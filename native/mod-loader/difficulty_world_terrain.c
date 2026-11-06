/* Extra large maps' terrain at vanilla density (difficulty_world.c's tiles, made the map's terrain).
 *
 * The level's plan has room for ~5700 Voronoi sites, so on a 16 km map they sit ~300 m apart and its terrain is a
 * few hundred flat blocks. Here the whole map is rebuilt as square tiles of TERRAIN_TILE_EXTENT metres, each one the
 * game's own Voronoi and height passes at vanilla's 16 m spacing (as the research tile, difficulty_world.c), and drawn
 * into the level's own terrain merge, so the heightmap the game smooths, captures, collides with and spawns as terrain
 * is the tiles' (at the merge's 4 m a sample on a 16 km map).
 *
 * A tile runs in tile-local coordinates ([0, extent], the only way GenerateVoronoi's square box allows), but what
 * makes terrain differ from place to place samples world positions: the fBm noise behind the height groups
 * (0xF3A2E0, plateau outlines) and the height texture GenerateSubregionHeights adds (sampled at 0xF64D89). While a tile builds,
 * both get the tile's world offset on the building thread; the texture is sampled at vanilla's scale (it covers a
 * vanilla world, 2048 m, once), repeating every 2048 m. The random draws (each record's LCG at +0x1990) are mixed with
 * the tile's own seed, so tiles differ in their lattice and routes. Then the tile's vertices and route-cell area points
 * move to its world position, and the level's RenderHeightmapVoronoi record (0x3C) runs again on the tile's data with
 * the level's merge (its pass +0 and +8 kept: the render object and the merge state with the level's target and
 * samples), its progress (pass +0x108 phase, +0x10C cell) from zero.
 *
 * The level waits at its SmoothHeightmap (0x40, right after its own 0x3C) until every tile within the playable disk is
 * drawn; tiles are built and drawn on the generating thread within a time budget per call. A tile pass that faults
 * costs that tile only (WorldTileGuarded); a tile that fails keeps the level's own cells there. Off with
 * HELLFORGE_XL_TERRAIN=0. */

#define TERRAIN_TILE_CORE 704        /* m: the part of a tile that is drawn */
#define TERRAIN_TILE_MARGIN 96       /* m on each side, built but not drawn: a tile flattens towards its box */
#define TERRAIN_TILE_EXTENT (TERRAIN_TILE_CORE + 2 * TERRAIN_TILE_MARGIN) /* 896 m: inside the route arrays' room */
#define TERRAIN_BUDGET_MS 40.0       /* per dispatcher call while the level waits */
#define TERRAIN_DRAW_CALLS 4096      /* per tile, across frames: RenderHeightmapVoronoi yields on its clock */

typedef float (*TerrainNoiseFn)(void* noise, float x, float y);
typedef uint64_t (*TerrainSampleFn)(void* texture, float* uv, void* out, uint64_t d);
static TerrainNoiseFn g_terrainNoise;
static TerrainSampleFn g_terrainSample;
static volatile DWORD g_terrainThread;   /* the thread building a tile (the samplers' offset applies there only) */
static float g_terrainOffset[2];
static volatile LONG g_nearActive; /* (below) the near tile's replay is running */
/* A parallel tile section holds the level's tile marker (lg+0x2D7C = disk ^ 1) and lattice spacing (16) for all its
 * tiles; TerrainBuild leaves them alone then (runners build at once). */
static volatile LONG g_tileSectionHeld;
/* The runners' engine jobs (TileRunnersStart) run g_tileWork until it returns; g_tileStop ends it early, g_tileFinished
 * counts the jobs that returned (TileRunnersJoin). */
static volatile LONG g_tileStop, g_tileFinished;
static void (*g_tileWork)(TileRunner* r);
enum { STAMP_SLOT_FREE, STAMP_SLOT_BUILT, STAMP_SLOT_RUNNING, STAMP_SLOT_DONE };
static volatile LONG g_tileJobsBroken; /* a runner did not return: one runner from then on */

static int TileThreads(void);
static int TileBuildsOn(void);
static int TileDispatchInstall(void);
static int TileRunnersStart(int count);
static void TileRunnersJoin(int started);

static struct
{
    LONG level;            /* the level this state is for */
    int state;             /* 0 not started, 1 running, 2 done */
    int tiles, columns, next, drawn, failed;
    int building;          /* the current tile is built and being drawn */
    uint32_t draw;         /* the level's RenderHeightmapVoronoi record */
    int drawCalls;
    uint8_t* block;        /* the tile's generator block */
    uint8_t* tmpl;         /* the level's block, relocated onto `block`, counts zeroed: copied in for each tile */
    uint32_t* offsets;     /* where tmpl holds a pointer into `block` (TerrainTemplate): a copy elsewhere moves them */
    uint32_t offsetCount;
    uint8_t* tmplBase;     /* the block tmpl's pointers point into (g_terrain.block when TerrainTemplate ran) */
    uint8_t record[0x19B0];
    uint8_t phase;
    double buildMs, drawMs;
    uint32_t frames;
    ULONGLONG started;
} g_terrain = {.level = -1};

/* The world offset of the tile this thread builds: a stamp planning runner's own (TileRunner), else the one tile the
 * generator thread builds; NULL when this thread builds none. */
static inline const float* TerrainOffsetSelf(void)
{
    TileRunner* r = TileRunnerSelf();
    if (r != NULL) return r->offsetOn ? r->offset : NULL;
    return g_terrainThread != 0 && g_terrainThread == GetCurrentThreadId() ? g_terrainOffset : NULL;
}
/* The generator block this thread builds tiles in: a runner's slot, else g_terrain.block. */
static inline uint8_t* TerrainBlockSelf(void)
{
    TileRunner* r = TileRunnerSelf();
    return r != NULL && r->block != NULL ? r->block : g_terrain.block;
}

/* The fBm noise (height groups' plateau outlines, GenerateSubregionHeights): world positions for a tile. */
static float TerrainNoise(void* noise, float x, float y)
{
    const float* o = TerrainOffsetSelf();
    if (o != NULL) { x += o[0]; y += o[1]; }
    return g_terrainNoise(noise, x, y);
}

/* The height texture's CPU image, as the sampler ([api+0x10]+0x1F0 = exe 0x31C8B0) reads it: {w u32, h u32, format i32,
 * ?, data at +0x10}, texel bits from the exe's table at 0x1658990. It is copied the first time a tile samples it in a
 * level, and tiles sample the copy: the near-player tile builds tiles again at the level's CleanupTerrains (0x65), and
 * the engine releases such images' CPU data during generation (a tile at op 0x6D read a null image at exe+0x31CA66). */
static struct { uint64_t key; uint8_t desc[0x20]; uint8_t* data; } g_terrainImages[4];
static LONG g_terrainImagesLevel = -1;

static void* TerrainImageLocked(void* texture)
{
    if (g_terrainImagesLevel != g_world.levels)
    {
        for (int k = 0; k < 4; k++) { if (g_terrainImages[k].data) VirtualFree(g_terrainImages[k].data, 0, MEM_RELEASE); memset(&g_terrainImages[k], 0, sizeof g_terrainImages[k]); }
        MemoryBarrier();
        g_terrainImagesLevel = g_world.levels;
    }
    uint64_t key = (uint64_t)(uintptr_t)texture;
    int k = 0;
    for (; k < 4 && g_terrainImages[k].key != 0; k++) if (g_terrainImages[k].key == key) return g_terrainImages[k].desc;
    if (k == 4 || texture == NULL) return texture;
    uint8_t desc[0x20];
    uint32_t w, h, format, bits = 0; uint64_t data;
    if (SafeRead(key, desc, sizeof desc)) return texture;
    memcpy(&w, desc, 4); memcpy(&h, desc + 4, 4); memcpy(&format, desc + 8, 4); memcpy(&data, desc + 0x10, 8);
    if (format >= 45 || SafeRead((uintptr_t)GetModuleHandleW(NULL) + 0x1658990u + 4u * format, &bits, 4) || bits < 8 || data == 0 ||
        w == 0 || h == 0 || w > 8192 || h > 8192) return texture;
    size_t bytes = (size_t)w * h * (bits / 8);
    uint8_t* copy = (uint8_t*)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (copy == NULL) return texture;
    if (SafeRead(data, copy, bytes)) { VirtualFree(copy, 0, MEM_RELEASE); return texture; }
    uint64_t mine = (uint64_t)(uintptr_t)copy;
    memcpy(desc + 0x10, &mine, 8);
    g_terrainImages[k].data = copy; memcpy(g_terrainImages[k].desc, desc, sizeof desc);
    MemoryBarrier();
    *(volatile uint64_t*)&g_terrainImages[k].key = key; /* published last */
    return g_terrainImages[k].desc;
}
/* Thread-safe (the stamp planning's runners build tiles at once): a lock-free look-up of the published copies (a key
 * is written last), the copy made under the lock. The level's reset runs before any tile of the level samples. */
static SRWLOCK g_terrainImagesLock = SRWLOCK_INIT;
static void* TerrainImage(void* texture)
{
    uint64_t key = (uint64_t)(uintptr_t)texture;
    if (g_terrainImagesLevel == g_world.levels)
        for (int k = 0; k < 4; k++) { uint64_t have = *(volatile uint64_t*)&g_terrainImages[k].key; if (have == 0) break; if (have == key) return g_terrainImages[k].desc; }
    TileRunner* self = TileRunnerSelf();
    LONG was = self != NULL ? self->where : 0;
    if (self != NULL) self->where = TILE_AT_IMAGE_WAIT;
    AcquireSRWLockExclusive(&g_terrainImagesLock);
    void* result = TerrainImageLocked(texture);
    ReleaseSRWLockExclusive(&g_terrainImagesLock);
    if (self != NULL) self->where = was;
    return result;
}

/* The height texture GenerateSubregionHeights adds: 0xF64C20 (called at 0xF6558C / 0xF656DC) samples it inline through
 * [api+0x10]+0x1F0 (exe 0x31C8B0) at 0xF64D89 with uv = position x the world's inverse size (0xF64D43 / 0xF64D5D; an
 * extra large world redirects the constant to 1 / its size). (0xF64B80, the wrapper this hooked before, has no callers:
 * tiles never got the offset.) For a tile, on its building thread: uv for the world position wrapped to a vanilla world
 * (the texture covers 2048 m once, as on a vanilla map), and the image copy. The render API slot is wrapped; other
 * callers pass through. */
static uint64_t TerrainSample(void* texture, float* uv, void* out, uint64_t d)
{
    const float* o = TerrainOffsetSelf();
    if (o != NULL && g_world.values != NULL &&
        (uintptr_t)__builtin_return_address(0) == g_world.game + 0xF64D90u && uv != NULL)
    {
        float p[2];
        for (int k = 0; k < 2; k++)
        {
            float w = fmodf(uv[k] * g_world.values->full + o[k], 2048.0f);
            if (w < 0) w += 2048.0f;
            p[k] = w / 2048.0f;
        }
        return g_terrainSample(TerrainImage(texture), p, out, d);
    }
    return g_terrainSample(texture, uv, out, d);
}

static int TerrainSampleInstall(void)
{
    if (g_terrainSample != NULL) return 1;
    uintptr_t api = 0, table = 0;
    if (SafeRead(g_world.game + 0x3326308u, &api, 8) || api == 0 || SafeRead(api + 0x10, &table, 8) || table == 0) return 0;
    if (!ApiCodeMatches(g_world.game + 0xF64D7Eu, "48 8B 05 83 15 3C 02 4C 8B 48 10 41 FF 91 F0 01 00 00"))
    { Note("difficulty: terrain: the height texture's sampler call is not the reviewed one; tiles sample it unshifted"); return 0; }
    void** slot = (void**)(uintptr_t)(table + 0x1F0);
    DWORD old = 0;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) return 0;
    g_terrainSample = (TerrainSampleFn)*slot;
    InterlockedExchangePointer(slot, (void*)TerrainSample);
    VirtualProtect(slot, 8, old, &old);
    return 1;
}

/* ---- Composite tiles meet without seams -------------------------------------------------------------------------------
 * RenderHeightmapVoronoi (0x3C, 0xF499B0) draws a tile's cells as gui triangles ([api+0xD0]+0x0 = exe 0x33D140: gui,
 * float3 p0, p1, p2 in merge pixels (metres from the world corner x lg+0x120 / settings+0x2C, y up) with the height as z,
 * layer, colour, material, uv x 3): phase 0 the Voronoi cells (vertex heights) and phase 1 the route-cell areas (fans to
 * the route points' heights), both with material 0x13DD2DCAD2968560 (layer terrain_height, additive), and the caps with
 * 0x369E29988294A16F (terrain_cap_height, MAX). A tile kept every cell whose centre was inside its 704 m core, whole:
 * across a boundary its cells and the neighbour's overlapped (added twice) or left gaps, and the two tiles' surfaces are
 * independent anyway (run v22 step dumps: a 4.3 m mean jump across the boundary before 0x40, 1.5 m after it against
 * 0.55 m elsewhere; a straight grid every 704 m). While a tile draws, its additive triangles are clipped to the core +-
 * TERRAIN_BLEND and their heights weighted by w = wx wy, each a linear ramp from 0 at TERRAIN_BLEND outside the core's
 * edge to 1 at TERRAIN_BLEND inside it (pieces split on the ramps' ends, so w is linear on each): neighbouring tiles
 * cross-fade over 2 TERRAIN_BLEND and the weights sum to 1. Caps (MAX of a constant) are clipped to the core exactly. */
#define TERRAIN_BLEND 32.0f
typedef void (*TerrainTriFn)(uint64_t gui, const float* p0, const float* p1, const float* p2, uint32_t layer, const float* colour,
    uint64_t material, const float* uv0, const float* uv1, const float* uv2);
static TerrainTriFn g_terrainTri;
static float MacroAt(float gx, float gy); /* difficulty_world_macro.c */
static struct { volatile DWORD thread; float lo[2], hi[2], px; uint64_t add, cap; uint64_t tris; } g_clip; /* tris: gui triangles while a clip is on (its thread only) */

/* The part of polygon p (n vertices: x, y, z) on one side of the line coordinate[axis] = v (keep >= v when above). */
static int TerrainClipSide(const float (*p)[3], int n, int axis, float v, int above, float (*out)[3])
{
    int m = 0;
    for (int i = 0; i < n; i++)
    {
        const float* a = p[i], *b = p[(i + 1) % n];
        float da = above ? a[axis] - v : v - a[axis], db = above ? b[axis] - v : v - b[axis];
        if (da >= 0) { memcpy(out[m++], a, 12); }
        if ((da >= 0) != (db >= 0))
        {
            float t = da / (da - db);
            for (int k = 0; k < 3; k++) out[m][k] = a[k] + t * (b[k] - a[k]);
            m++;
        }
    }
    return m;
}

static float TerrainRamp(float u, float lo, float hi)
{
    float a = (u - (lo - TERRAIN_BLEND)) / (2 * TERRAIN_BLEND), b = ((hi + TERRAIN_BLEND) - u) / (2 * TERRAIN_BLEND);
    a = a < 0 ? 0 : a > 1 ? 1 : a; b = b < 0 ? 0 : b > 1 ? 1 : b;
    return a < b ? a : b;
}

/* Splits on the ramps' ends (4 lines), then draws each piece as a fan with its heights weighted. Coordinates in pixels. */
static void TerrainClipEmit(float (*p)[3], int n, int split, uint64_t gui, uint32_t layer, const float* colour, uint64_t material)
{
    if (n < 3) return;
    if (split < 4)
    {
        int axis = split & 1;
        float v = (split < 2 ? g_clip.lo[axis] + TERRAIN_BLEND : g_clip.hi[axis] - TERRAIN_BLEND) * g_clip.px;
        float a[24][3], b[24][3];
        int na = TerrainClipSide((const float (*)[3])p, n, axis, v, 0, a), nb = TerrainClipSide((const float (*)[3])p, n, axis, v, 1, b);
        TerrainClipEmit(a, na, split + 1, gui, layer, colour, material);
        TerrainClipEmit(b, nb, split + 1, gui, layer, colour, material);
        return;
    }
    for (int i = 0; i < n; i++)
        p[i][2] *= TerrainRamp(p[i][0] / g_clip.px, g_clip.lo[0], g_clip.hi[0]) * TerrainRamp(p[i][1] / g_clip.px, g_clip.lo[1], g_clip.hi[1]);
    for (int i = 1; i + 1 < n; i++)
    {
        float area = (p[i][0] - p[0][0]) * (p[i + 1][1] - p[0][1]) - (p[i + 1][0] - p[0][0]) * (p[i][1] - p[0][1]);
        if (fabsf(area) < 1e-6f) continue;
        g_clip.tris++;
        g_terrainTri(gui, p[0], p[i], p[i + 1], layer, colour, material, NULL, NULL, NULL);
    }
}

static void TerrainTri(uint64_t gui, const float* p0, const float* p1, const float* p2, uint32_t layer, const float* colour, uint64_t material,
    const float* uv0, const float* uv1, const float* uv2)
{
    if (g_clip.thread == 0 || g_clip.thread != GetCurrentThreadId() || (material != g_clip.add && material != g_clip.cap) || uv0 != NULL)
    {
        if (g_clip.thread != 0 && g_clip.thread == GetCurrentThreadId()) g_clip.tris++;
        g_terrainTri(gui, p0, p1, p2, layer, colour, material, uv0, uv1, uv2);
        return;
    }
    float a[24][3] = {{p0[0], p0[1], p0[2]}, {p1[0], p1[1], p1[2]}, {p2[0], p2[1], p2[2]}}, b[24][3];
    int n = 3;
    float grow = material == g_clip.add ? TERRAIN_BLEND : 0.0f;
    for (int axis = 0; axis < 2 && n >= 3; axis++)
    {
        n = TerrainClipSide((const float (*)[3])a, n, axis, (g_clip.lo[axis] - grow) * g_clip.px, 1, b);
        n = TerrainClipSide((const float (*)[3])b, n, axis, (g_clip.hi[axis] + grow) * g_clip.px, 0, a);
    }
    if (n < 3) return;
    if (material == g_clip.add) { TerrainClipEmit(a, n, 0, gui, layer, colour, material); return; }
    /* a near tile's replay: its base (the level's capture) holds the km-scale relief, so its caps are lifted by it */
    if (g_nearActive) for (int i = 0; i < n; i++) a[i][2] += MacroAt(a[i][0] / g_clip.px, a[i][1] / g_clip.px);
    for (int i = 1; i + 1 < n; i++) { g_clip.tris++; g_terrainTri(gui, a[0], a[i], a[i + 1], layer, colour, material, NULL, NULL, NULL); }
}

static int TerrainTriInstall(void)
{
    if (g_terrainTri != NULL) return 1;
    uintptr_t api = 0, table = 0;
    if (SafeRead(g_world.game + 0x3326308u, &api, 8) || api == 0 || SafeRead(api + 0xD0, &table, 8) || table == 0) return 0;
    void** slot = (void**)(uintptr_t)table;
    DWORD old = 0;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) return 0;
    g_terrainTri = (TerrainTriFn)*slot;
    InterlockedExchangePointer(slot, (void*)TerrainTri);
    VirtualProtect(slot, 8, old, &old);
    return 1;
}

/* Around one composite tile's draw on this thread: its core (generator metres) and the merge's pixel scale. 0 = no clip. */
static int TerrainClipBegin(int tile, uintptr_t gen)
{
    typedef uint64_t (*MatFn)(uint64_t gui, uint64_t hash);
    uint64_t R = 0, gui = 0; float samples = 0;
    MatFn mat = NULL;
    { uintptr_t api = 0, t = 0, f = 0;
      if (!SafeRead(g_world.game + 0x3326308u, &api, 8) && api && !SafeRead(api + 0xD0, &t, 8) && t && !SafeRead(t + 0x230, &f, 8)) mat = (MatFn)f; }
    if (!TerrainTriInstall() || mat == NULL || SafeRead(gen, &R, 8) || R == 0 || SafeRead(R + 0x110, &gui, 8) || gui == 0 ||
        SafeRead(gen + 0x8758A8u + 0x120u, &samples, 4) || !(samples >= 1024.0f && samples <= 8192.0f)) return 0;
    const float core = (float)TERRAIN_TILE_CORE;
    g_clip.lo[0] = (float)(tile % g_terrain.columns) * core; g_clip.lo[1] = (float)(tile / g_terrain.columns) * core;
    g_clip.hi[0] = g_clip.lo[0] + core; g_clip.hi[1] = g_clip.lo[1] + core;
    g_clip.px = samples / 16384.0f; /* both callers run only on a 16384 m world (settings+0x2C checked) */
    g_clip.add = mat(gui, 0x13DD2DCAD2968560ull); g_clip.cap = mat(gui, 0x369E29988294A16Full);
    MemoryBarrier();
    g_clip.thread = GetCurrentThreadId();
    return 1;
}

static void TerrainClipEnd(void) { g_clip.thread = 0; }

static int TerrainOn(void)
{
    static int on = -1;
    if (on < 0) { char t[8] = ""; on = !(GetEnvironmentVariableA("HELLFORGE_XL_TERRAIN", t, sizeof t) > 0 && t[0] == '0'); }
    return on && g_terrainNoise != NULL;
}

static uint64_t TerrainSeed(int tile)
{
    uint64_t z = (uint64_t)(tile + 1) * 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* The level's block as every tile starts: copied, its pointers moved onto the tile's block, counts zeroed and the
 * route-cell areas cleared (difficulty_world.c, WorldTileProbe: stale 16 km entries otherwise reach the render). */
static int TerrainTemplate(uintptr_t gen)
{
    uint32_t size = GEN_BLOCK_SIZE;
    if (g_terrain.block == NULL) g_terrain.block = (uint8_t*)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_terrain.tmpl == NULL) g_terrain.tmpl = (uint8_t*)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_terrain.block == NULL || g_terrain.tmpl == NULL || SafeRead(gen, g_terrain.tmpl, size)) return 0;
    WorldTileRelocateIn(g_terrain.tmpl, size, gen, (uintptr_t)g_terrain.block);
    for (size_t k = 0; k < sizeof kTileCounts / sizeof kTileCounts[0]; k++) memset(g_terrain.tmpl + kTileCounts[k], 0, 4);
    /* the route points (G+0xB47C78, 0x90 each: 16 segment slots, xy at +0x80, height at +0x88) and the route-cell area
     * points after them (G+0xEE10C8). AddRoutePoints (0x03, 0xF93800) makes a new point j (in route-cell order, so
     * along tile-local y) by writing only its xy and slot 0, and GenerateSubregionHeights (0x04, 0xF64E30) sets +0x88
     * only for points it reaches through a subregion it does not skip (0xF656F9); RenderHeightmapVoronoi draws every
     * subregion's fans to [point]+0x88 (0xF49FED / 0xF4A004). Uncleared, every point 0x04 did not reach kept the
     * level's height for the same j: the same per-row heights in every tile, a step at each 704 m tile boundary along y
     * (run v21 step dumps: composite add 12-13 m at local y 620-690, 2-5 m at 50-180, no pattern along x). */
    memset(g_terrain.tmpl + 0xB47C78u, 0, 0x10B1F48u - 0xB47C78u);
    /* the level's subregions (G+0x92F328, 1024 x 0x20, up to G+0x937328): 0x75 resets only +8 and +0x18, and
     * SelectSubRegionCellsFromHeightGroup (0x78) keeps a non-zero type at +0 (0xF37A80 -> 0xF37CEE), so every tile took
     * the level's types by index (route cells, and so subregions, run row by row in y: the 704 m band along y) */
    memset(g_terrain.tmpl + 0x92F328u, 0, 0x8000u);
    /* the level's vertex flags (u16, G+0x10BB7C, 5849 up to G+0x10E92E): only FinalizeLocationPlacements (0x8D, not run
     * on tiles) sets bit 2, and RenderHeightmapVoronoi's phase 0 flattens cells on it (0xF49C27, 0xF49D80) */
    memset(g_terrain.tmpl + 0x10BB7Cu, 0, 0x10E92Eu - 0x10BB7Cu);
    if (!TerrainSampleInstall()) Note("difficulty: terrain: the height texture's sampler is not wrapped; tiles sample it unshifted");
    /* the pointers into the block as the template holds them (after the clears above), so a tile can be built in
     * another block (the stamp planning's slots, TerrainCopyTemplate) */
    if (g_terrain.offsets == NULL) g_terrain.offsets = (uint32_t*)VirtualAlloc(NULL, (size_t)(size / 8u) * 4u, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_terrain.offsetCount = 0; g_terrain.tmplBase = NULL;
    if (g_terrain.offsets != NULL)
    {
        const uint64_t from = (uint64_t)(uintptr_t)g_terrain.block, to = from + size;
        for (uint32_t o = 0; o + 8 <= size; o += 8)
        {
            uint64_t v; memcpy(&v, g_terrain.tmpl + o, 8);
            if (v >= from && v < to) g_terrain.offsets[g_terrain.offsetCount++] = o;
        }
        g_terrain.tmplBase = g_terrain.block;
        Note("difficulty: terrain: the tile template holds %u pointers into its block", g_terrain.offsetCount);
    }
    return 1;
}

/* The template into a block: copied, its pointers moved from the block TerrainTemplate made it for onto `T`. 0 when
 * `T` is another block and the list is missing. */
static int TerrainCopyTemplate(uint8_t* T)
{
    uint32_t size = GEN_BLOCK_SIZE;
    uint8_t* base = g_terrain.offsets != NULL && g_terrain.tmplBase != NULL ? g_terrain.tmplBase : g_terrain.block;
    if (T == base) { memcpy(T, g_terrain.tmpl, size); return 1; }
    if (g_terrain.offsets == NULL || g_terrain.tmplBase == NULL) return 0;
    memcpy(T, g_terrain.tmpl, size);
    const uint64_t delta = (uint64_t)(uintptr_t)T - (uint64_t)(uintptr_t)base;
    for (uint32_t k = 0; k < g_terrain.offsetCount; k++) *(uint64_t*)(T + g_terrain.offsets[k]) += delta;
    return 1;
}

/* A tile's open land takes the level's zones. The template clears the tile's subregions (G+0x92F328), so the tile's
 * SelectSubRegionCellsFromHeightGroup (0x78) rolls each cell's zone from the level's zone list. On a city map every
 * non-city entry there weighs 0, and the level's open land got its zones from WorldCityOpenZones (before the level's
 * 0x78, which keeps a non-zero zone at +0: 0xF37A80 -> 0xF37CEE). So the tiles' open land was city zones: city types,
 * whose weight rules place nothing outside a city (s74s, a vanilla city map: 0 stamps from 0x2B), and s75t's 169 stamp
 * tiles made 0 records over 189-199 items each. Before the tile's 0x78, each of its cells (+8 == -1, as the level's
 * call takes) whose nearest level cell (by route-cell point, +0x18 -> xy, generator metres) is not a city cell (the
 * cell's type +0xC, +0x170 bit 1) takes that cell's zone id (+0); the rest roll as before. Only on a level with cities. */
static struct { LONG built; float lx[1024], ly[1024]; uint32_t lz[1024]; uint8_t lcity[1024]; uint32_t levelCells; LONG stats[3]; } g_tileZones = {.built = -1};
/* The level's cells for TerrainTileZones, once a level; on the generator thread before any runner builds a tile. */
static void TerrainTileZonesPrepare(uintptr_t gen)
{
    if (g_cities.level != g_world.levels || g_cities.count <= 0 || g_worldLevel == 0) return;
    float* lx = g_tileZones.lx; float* ly = g_tileZones.ly; uint32_t* lz = g_tileZones.lz; uint8_t* lcity = g_tileZones.lcity;
    uint32_t levelCells;
    LONG* stats = g_tileZones.stats; /* tiles, cells written, cells left to roll */
    if (g_tileZones.built != g_world.levels)
    {
        uint64_t ts = 0, table = 0; uint32_t types = 0, subs = 0;
        levelCells = 0;
        if (SafeRead(g_worldLevel + 0x2D0, &ts, 8) || ts == 0 || SafeRead(ts, &table, 8) || table == 0 || SafeRead(ts + 0xA0, &types, 4) ||
            SafeRead(gen + 0x11A71C4u, &subs, 4) || subs > 1024) return;
        for (uint32_t i = 0; i < subs; i++)
        {
            uintptr_t sub = gen + 0x92F328u + 0x20ull * i;
            uint64_t route = 0, d = 0; float p[2]; int32_t t = -1; uint32_t zone = 0, flags = 0;
            if (SafeRead(sub, &zone, 4) || SafeRead(sub + 0xC, &t, 4) || SafeRead(sub + 0x18, &route, 8) || route == 0 || SafeRead(route, p, 8)) continue;
            lcity[levelCells] = t >= 0 && (uint32_t)t < types && !SafeRead(table + 8ull * (uint32_t)t, &d, 8) && d != 0 && !SafeRead(d + 0x170, &flags, 4) && (flags & 2u);
            lx[levelCells] = p[0]; ly[levelCells] = p[1]; lz[levelCells] = zone; levelCells++;
        }
        g_tileZones.levelCells = levelCells;
        stats[0] = stats[1] = stats[2] = 0;
        MemoryBarrier();
        g_tileZones.built = g_world.levels;
    }
}
static void TerrainTileZones(uintptr_t gen, float ox, float oy)
{
    if (g_cities.level != g_world.levels || g_cities.count <= 0 || g_worldLevel == 0) return;
    if (g_tileZones.built != g_world.levels && TileRunnerSelf() == NULL) TerrainTileZonesPrepare(gen);
    if (g_tileZones.built != g_world.levels) return;
    const float* lx = g_tileZones.lx; const float* ly = g_tileZones.ly; const uint32_t* lz = g_tileZones.lz; const uint8_t* lcity = g_tileZones.lcity;
    const uint32_t levelCells = g_tileZones.levelCells;
    LONG* stats = g_tileZones.stats;
    uint8_t* T = TerrainBlockSelf();
    if (levelCells == 0 || T == NULL) return;
    uint32_t cells = 0; memcpy(&cells, T + 0x11A71C4, 4);
    if (cells > 1024) return;
    uint32_t written = 0, left = 0;
    for (uint32_t i = 0; i < cells; i++)
    {
        uint8_t* sub = T + 0x92F328 + (size_t)i * 0x20;
        int32_t special = 0; uint64_t route = 0; float p[2];
        memcpy(&special, sub + 8, 4); memcpy(&route, sub + 0x18, 8);
        if (special != -1 || route == 0 || SafeRead(route, p, 8)) continue;
        float gx = p[0] + ox, gy = p[1] + oy, best = 3.4e38f; uint32_t at = 0;
        for (uint32_t k = 0; k < levelCells; k++) { float dx = lx[k] - gx, dy = ly[k] - gy, d = dx * dx + dy * dy; if (d < best) { best = d; at = k; } }
        if (lcity[at] || lz[at] == 0) { left++; continue; }
        memcpy(sub, &lz[at], 4); written++;
    }
    LONG n = InterlockedIncrement(&stats[0]); InterlockedExchangeAdd(&stats[1], (LONG)written); InterlockedExchangeAdd(&stats[2], (LONG)left);
    if (n == 1 || n == 16 || n == 169 || n % 1000 == 0)
        Note("difficulty: terrain: tiles take the level's open zones: %ld tiles so far, %ld cells given the level's zone, %ld left to roll (city)", (long)n, (long)stats[1], (long)stats[2]);
}

/* One tile: the game's GenerateVoronoi and height passes on the tile's block, then (move) moved to its world position.
 * 1 when built. Without move the tile stays in its own frame (the stamp rings run the level's op 0x2B on it). */
static int TerrainBuild(int tile, uint64_t records, uint32_t end, uint64_t context, uint64_t g, uint64_t h, uintptr_t gen, int move)
{
    const float extent = (float)TERRAIN_TILE_EXTENT, core = (float)TERRAIN_TILE_CORE;
    float cx = (float)(tile % g_terrain.columns) * core, cy = (float)(tile / g_terrain.columns) * core;
    float ox = cx - (float)TERRAIN_TILE_MARGIN, oy = cy - (float)TERRAIN_TILE_MARGIN;
    uintptr_t settings = 0, lg = 0, p = 0, spacingAt = 0;
    uint32_t disk = 0;
    if (SafeRead(context + 8, &settings, 8) || settings == 0 || SafeRead(context + 0x18, &lg, 8) || lg == 0 ||
        SafeRead(lg + 0x2D0, &p, 8) || p == 0 || SafeRead(p + 0x68, &spacingAt, 8) || spacingAt == 0 || SafeRead(lg + 0x2D7C, &disk, 4))
        return 0;
    /* a stamp planning runner builds in its own slot with its own record copy and context (settings +8 its own) */
    TileRunner* runner = TileRunnerSelf();
    uint8_t* B = TerrainBlockSelf();
    if (B == NULL || !TerrainCopyTemplate(B)) return 0;
    uint64_t seed = TerrainSeed(tile);
    static uint8_t oneGenerator[0x19B0];
    uint8_t* one = runner != NULL ? runner->one : oneGenerator;
    if (SafeRead(g_rec25, one, 0x19B0)) return 0;
    WorldTileRelocate(one, gen, (uintptr_t)B);
    { uint64_t r; memcpy(&r, one + 0x1990, 8); r ^= seed; memcpy(one + 0x1990, &r, 8); }

    volatile uint32_t* sizeP = (volatile uint32_t*)(settings + 0x2C);
    volatile float* loP = (volatile float*)(settings + 0x64);
    volatile float* hiP = (volatile float*)(settings + 0x68);
    volatile float* radiusP = (volatile float*)(settings + 0x6C);
    volatile float* spacingP = (volatile float*)spacingAt;
    volatile uint32_t* diskP = (volatile uint32_t*)(lg + 0x2D7C);
    uint32_t size = *sizeP;
    float lo = *loP, hi = *hiP, radius = *radiusP, spacing = *spacingP;
    {
        static LONG noted[2] = {-1, -1};
        int k = g_nearActive ? 1 : 0;
        if (noted[k] != g_world.levels)
        {
            noted[k] = g_world.levels;
            uint32_t s20 = 0, s3C = 0; float f1C = 0;
            SafeRead(settings + 0x20, &s20, 4); SafeRead(settings + 0x3C, &s3C, 4); SafeRead(settings + 0x1C, &f1C, 4);
            Note("difficulty: terrain: tile %d built %s: disk %u, size %u, lo %.1f, hi %.1f, radius %.1f, spacing %.1f, settings+0x1C %.4f +0x20 0x%X +0x3C 0x%X, rec25 at %u",
                tile, k ? "for the near tile" : "for the level", disk, size, (double)lo, (double)hi, (double)radius, (double)spacing, (double)f1C, s20, s3C,
                (unsigned)((g_rec25 - records) / 0x19B0u));
        }
    }
    /* the tile marker and the lattice spacing are the level's (lg): a parallel section holds them for all its tiles */
    const int held = g_tileSectionHeld;
    if (!held) { *diskP = disk ^ 1u; *spacingP = 16.0f; }
    *sizeP = (uint32_t)extent; *loP = 0.0f; *hiP = extent; *radiusP = extent;
    if (runner != NULL) { runner->offset[0] = ox; runner->offset[1] = oy; runner->offsetOn = 1; }
    else { g_terrainOffset[0] = ox; g_terrainOffset[1] = oy; g_terrainThread = GetCurrentThreadId(); }

    typedef void (*Handler)(uint64_t, uint64_t, uint64_t);
    if (runner != NULL) { runner->tile = tile; runner->op = 0x25; runner->where = TILE_AT_VORONOI; }
    ((Handler)(g_world.game + 0xF3F8E0u))((uint64_t)(uintptr_t)one + 8, (uint64_t)(uintptr_t)one, context);
    WorldArenaReset(context); /* GenerateVoronoi is done: the arena as the game leaves it after an op */
    int ok = 1, seen04 = 0;
    uint32_t start = 0xFFFFFFFFu, op = 0;
    for (uint32_t i = 0; i < end; i++)
        if (!SafeRead(records + (uint64_t)i * 0x19B0u, &op, 4) && op == 0x25) { start = i + 1; break; }
    if (start == 0xFFFFFFFFu) ok = 0;
    for (uint32_t i = start; ok && i < end; i++)
    {
        if (SafeRead(records + (uint64_t)i * 0x19B0u, one, 0x19B0)) { ok = 0; break; }
        memcpy(&op, one, 4);
        if (!WorldTileRuns(op)) continue;
        WorldTileRelocate(one, gen, (uintptr_t)B);
        { uint64_t r; memcpy(&r, one + 0x1990, 8); r ^= seed; memcpy(one + 0x1990, &r, 8); }
        if (op == 0x78) { if (runner != NULL) runner->where = TILE_AT_ZONES; TerrainTileZones(gen, ox, oy); }
        volatile uint32_t index = 0;
        volatile int calls = 0;
        uint8_t phase = 0;
        LONG fault = WorldTileGuarded(one, context, &index, &phase, g, h, 10000, &calls);
        if (fault || index == 0 || TileGuardSelf()->weldFailed)
        {
            Note("difficulty: terrain: tile %d (%.0f, %.0f) %s in op 0x%02X%s", tile, (double)ox, (double)oy,
                fault ? "faulted" : TileGuardSelf()->weldFailed ? "failed its weld" : "did not finish", op, fault || TileGuardSelf()->weldFailed ? "; the level's own cells stay there" : "");
            ok = 0;
            break;
        }
        WorldArenaReset(context);
        if (op == 0x04) seen04 = 1;
        if (op == 0x2D && seen04) break;
    }
    if (runner != NULL) runner->offsetOn = 0; else g_terrainThread = 0;
    *sizeP = size; *loP = lo; *hiP = hi; *radiusP = radius;
    if (!held) { *spacingP = spacing; *diskP = disk; }
    if (runner != NULL) runner->where = TILE_AT_MOVE;
    if (!ok || !seen04) return 0;
    if (!move) return 1;

    /* to the tile's world position: the Voronoi vertices (x, y at +0) and the route-cell areas' points (x, y at +0x80)
     * the route cells name ([+0xA0, +0xA4)) */
    uint8_t* T = B;
    uint32_t vertices = 0, cells = 0, last = 0;
    memcpy(&vertices, T + 0x11A7090, 4);
    memcpy(&cells, T + 0x11A71C0, 4);
    if (vertices > 6000 || cells > 4096) return 0;
    {
        /* research: what a tile's Voronoi holds, the first tile at level time and the first at the near tile's replay */
        int k = g_nearActive ? 1 : 0, col = tile % g_terrain.columns, row = tile / g_terrain.columns;
        if (k || (col >= 10 && col <= 13 && row >= 10 && row <= 13)) /* the near tile's (centred at 0, 0) at both times */
        {
            float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
            for (uint32_t v = 0; v < vertices; v++)
            {
                float xy[2]; memcpy(xy, T + 0x9F310 + (size_t)v * 0x48, 8);
                for (int a = 0; a < 2; a++) { if (xy[a] < lo[a]) lo[a] = xy[a]; if (xy[a] > hi[a]) hi[a] = xy[a]; }
            }
            uint32_t cellCount0 = 0; memcpy(&cellCount0, T + 0x11A70B8, 4);
            Note("difficulty: terrain: tile %d (%s): %u vertices over x %.0f..%.0f, y %.0f..%.0f (tile-local), %u cells, %u route cells", tile,
                k ? "near replay" : "level", vertices, (double)lo[0], (double)hi[0], (double)lo[1], (double)hi[1], cellCount0, cells);
        }
    }
    for (uint32_t v = 0; v < vertices; v++)
    {
        float xy[2];
        memcpy(xy, T + 0x9F310 + (size_t)v * 0x48, 8);
        xy[0] += ox; xy[1] += oy;
        memcpy(T + 0x9F310 + (size_t)v * 0x48, xy, 8);
    }
    for (uint32_t c = 0; c < cells; c++)
    {
        uint32_t r[2];
        memcpy(r, T + 0x8EB328 + (size_t)c * 0x110 + 0xA0, 8);
        if (r[1] >= r[0] && r[1] <= 13224u && r[1] > last) last = r[1];
    }
    /* RenderHeightmapVoronoi's second phase draws from each point along its 16 segment pointers (+0 .. +0x78), and a
     * segment holds its own two ends (x, y at +0x20 and +0x28; heights through its point pointers +0 / +8): each
     * segment those points name moves once */
    static uint8_t* movedGenerator;
    uint32_t blockSize = GEN_BLOCK_SIZE;
    uint8_t** movedAt = runner != NULL ? &runner->moved : &movedGenerator;
    if (*movedAt == NULL) *movedAt = (uint8_t*)VirtualAlloc(NULL, blockSize / 64 + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t* moved = *movedAt;
    if (moved == NULL) return 0;
    memset(moved, 0, blockSize / 64 + 1);
    for (uint32_t k = 0; k < last; k++)
    {
        uint8_t* point = T + 0xEE10C8 + (size_t)k * 0x90;
        float xy[2];
        memcpy(xy, point + 0x80, 8);
        xy[0] += ox; xy[1] += oy;
        memcpy(point + 0x80, xy, 8);
        for (int j = 0; j < 16; j++)
        {
            uint64_t s;
            memcpy(&s, point + 8 * j, 8);
            if (s < (uint64_t)(uintptr_t)T || s + 0x30 > (uint64_t)(uintptr_t)T + blockSize) continue;
            uint64_t at = s - (uint64_t)(uintptr_t)T;
            uint64_t slot = at / 8; /* a bit per 8-byte slot of the block: each segment moves once */
            if (moved[slot / 8] & (1u << (slot % 8))) continue;
            moved[slot / 8] |= (uint8_t)(1u << (slot % 8));
            float ends[4];
            memcpy(ends, T + at + 0x20, 16);
            ends[0] += ox; ends[1] += oy; ends[2] += ox; ends[3] += oy;
            memcpy(T + at + 0x20, ends, 16);
        }
    }
    /* Only the core and its blend band are drawn (TerrainTri clips each triangle there): a cell whose centre (its edges'
     * ends, as the render finds it) lies more than a cell (24 m) beyond the band loses its edges, which the render skips; a
     * route-cell area point there loses its segments. Kept cells stay inside the built box (the margin is 96 m). */
    const float keepLo[2] = {cx - TERRAIN_BLEND - 24.0f, cy - TERRAIN_BLEND - 24.0f}, keepHi[2] = {cx + core + TERRAIN_BLEND + 24.0f, cy + core + TERRAIN_BLEND + 24.0f};
    uint32_t cellCount = 0;
    memcpy(&cellCount, T + 0x11A70B8, 4);
    if (cellCount > 6000) return 0;
    for (uint32_t c = 0; c < cellCount; c++)
    {
        uint8_t* cell = T + 0x129BD0 + (size_t)c * 0x88;
        double sx = 0, sy = 0;
        int n = 0;
        for (int k = 0; k < 16; k++)
        {
            uint64_t e;
            memcpy(&e, cell + 8 + 8 * k, 8);
            if (e < (uint64_t)(uintptr_t)T || e + 16 > (uint64_t)(uintptr_t)T + blockSize) continue;
            uint64_t ends[2];
            memcpy(ends, (uint8_t*)(uintptr_t)e, 16);
            for (int q = 0; q < 2; q++)
            {
                if (ends[q] < (uint64_t)(uintptr_t)T || ends[q] + 8 > (uint64_t)(uintptr_t)T + blockSize) continue;
                float v[2];
                memcpy(v, (uint8_t*)(uintptr_t)ends[q], 8);
                sx += v[0]; sy += v[1]; n++;
            }
        }
        if (n == 0) continue;
        sx /= n; sy /= n;
        if (sx < keepLo[0] || sx >= keepHi[0] || sy < keepLo[1] || sy >= keepHi[1]) memset(cell + 8, 0, 16 * 8);
    }
    for (uint32_t k = 0; k < last; k++)
    {
        uint8_t* point = T + 0xEE10C8 + (size_t)k * 0x90;
        float xy[2];
        memcpy(xy, point + 0x80, 8);
        if (xy[0] < keepLo[0] || xy[0] >= keepHi[0] || xy[1] < keepLo[1] || xy[1] >= keepHi[1]) memset(point, 0, 16 * 8);
    }
    return 1;
}

/* The tile's draw: the level's RenderHeightmapVoronoi record on the tile's block, keeping the level's render object
 * (pass +0) and merge (pass +8), its progress from zero. */
static int TerrainDrawPrepare(uint64_t records, uintptr_t gen)
{
    if (SafeRead(records + (uint64_t)g_terrain.draw * 0x19B0u, g_terrain.record, sizeof g_terrain.record)) return 0;
    uint64_t keep[2];
    memcpy(keep, g_terrain.record + 8, 16);
    WorldTileRelocate(g_terrain.record, gen, (uintptr_t)g_terrain.block);
    memcpy(g_terrain.record + 8, keep, 16);
    memset(g_terrain.record + 8 + 0x108, 0, 8);
    g_terrain.phase = 0;
    g_terrain.drawCalls = 0;
    return 1;
}

static int TerrainTileWanted(int tile)
{
    /* within the playable disk (centre of the world square, the level's keep radius) plus one tile */
    const double extent = TERRAIN_TILE_CORE, world = 16384.0, radius = 8192.0 + extent;
    double x0 = (tile % g_terrain.columns) * extent, y0 = (tile / g_terrain.columns) * extent, c = world / 2.0;
    double dx = c < x0 ? x0 - c : (c > x0 + extent ? c - (x0 + extent) : 0.0);
    double dy = c < y0 ? y0 - c : (c > y0 + extent ? c - (y0 + extent) : 0.0);
    return dx * dx + dy * dy <= radius * radius;
}

/* ---- The coarse terrain's tiles in parallel (load time) -------------------------------------------------------------
 * As the stamp planning's stage B: runners (engine jobs) build the wanted tiles (TerrainBuild with move, then the draw's
 * record on the slot's block) into slots in tile order, each with its own context (the level's settings as the section
 * holds them: the tile marker and lattice spacing set once for all tiles); the main thread draws them in tile order
 * within its frame budget (RenderHeightmapVoronoi on the level's merge, as the serial code does), so the merge sees the
 * serial order. Off with HELLFORGE_XL_TILE_THREADS=1 or HELLFORGE_XL_TILE_BUILDS=0. */
#define COARSE_SLOTS_MAX (TILE_RUNNERS_MAX + 4)
typedef struct
{
    volatile LONG state;        /* STAMP_SLOT_FREE, _RUNNING (building), _BUILT (ready to draw) */
    int k, t, ok;
    uint8_t* T;
    uint8_t record[0x19B0];     /* the level's RenderHeightmapVoronoi record on this block */
    uint8_t phase;
    int drawCalls;
    double buildMs;
    uint64_t tris;              /* gui triangles its draw calls emitted */
} CoarseSlot;
static struct
{
    int on, count, draw, slotCount, started;
    LONG level;                 /* the level the section is for: its runners stop when another level begins */
    ULONGLONG waitSince; int stallNoted;
    int list[1024];
    volatile LONG nextTile, slotOf[1024];
    CoarseSlot slots[COARSE_SLOTS_MAX];
    uint8_t* blocks[COARSE_SLOTS_MAX];
    uint64_t records, context, g, h; uint32_t end; uintptr_t gen, P;
    uint8_t ctx[0xE8], buildS[0x120];
    uintptr_t lg, spacingAt; uint32_t disk; float spacing;
    uint64_t submit;            /* the engine's job submit ([[game+0x33263E8]+0x88]) */
    uint64_t element[TILE_RUNNERS_MAX]; /* each runner's job element (its TileRunner), fixed while the section runs */
    volatile LONG busy[TILE_RUNNERS_MAX], inflight, pending;
    long jobs, selfBuilt, queueWaits;
    uint64_t tileTrisMax, tileTrisSum, frameTrisMax; long tilesCounted;
} g_coarse;

/* One coarse tile on a runner (an engine job, CoarseFeed): a free slot and the next tile, built, then the job returns.
 * A job never waits: an engine worker held by a waiting job starves the main thread's own job waits (t77f/t77g: runners
 * sleeping for a slot while the main thread, blocked in the job system's wait, never drew a tile to free one). */
static int CoarseOne(TileRunner* r, int job)
{
    CoarseSlot* s = NULL;
    for (int i = 0; i < g_coarse.slotCount && s == NULL; i++)
        if (g_coarse.slots[i].state == STAMP_SLOT_FREE && InterlockedCompareExchange(&g_coarse.slots[i].state, STAMP_SLOT_RUNNING, STAMP_SLOT_FREE) == STAMP_SLOT_FREE)
            s = &g_coarse.slots[i];
    LONG k = s != NULL ? InterlockedIncrement(&g_coarse.nextTile) - 1 : 0;
    if (job) InterlockedDecrement(&g_coarse.pending); /* claimed (or none to claim): CoarseFeed counts it by the slot from here */
    if (s == NULL) return 0;
    r->where = TILE_AT_BUILD;
    if (k >= g_coarse.count) { InterlockedExchange(&s->state, STAMP_SLOT_FREE); r->where = TILE_AT_IDLE; return 0; }
    s->k = (int)k; s->t = g_coarse.list[k];
    LARGE_INTEGER hz, b0, b1; QueryPerformanceFrequency(&hz); QueryPerformanceCounter(&b0);
    r->block = s->T;
    TileRunnerContext(r, g_coarse.ctx, g_coarse.buildS, g_coarse.P);
    s->ok = TerrainBuild(s->t, g_coarse.records, g_coarse.end, (uint64_t)(uintptr_t)r->ctx, g_coarse.g, g_coarse.h, g_coarse.gen, 1);
    r->block = NULL;
    r->where = TILE_AT_PREPARE;
    if (s->ok)
    {
        /* the draw's record (TerrainDrawPrepare) on this slot's block */
        s->ok = !SafeRead(g_coarse.records + (uint64_t)g_coarse.draw * 0x19B0u, s->record, sizeof s->record);
        if (s->ok)
        {
            uint64_t keep[2];
            memcpy(keep, s->record + 8, 16);
            WorldTileRelocate(s->record, g_coarse.gen, (uintptr_t)s->T);
            memcpy(s->record + 8, keep, 16);
            memset(s->record + 8 + 0x108, 0, 8);
        }
    }
    s->phase = 0; s->drawCalls = 0; s->tris = 0;
    QueryPerformanceCounter(&b1); s->buildMs = (double)(b1.QuadPart - b0.QuadPart) * 1000.0 / (double)hz.QuadPart;
    MemoryBarrier();
    InterlockedExchange(&g_coarse.slotOf[k], (LONG)(s - g_coarse.slots));
    InterlockedExchange(&s->state, STAMP_SLOT_BUILT);
    r->where = TILE_AT_IDLE;
    return 1;
}

static uint64_t CoarseJob(uint64_t a, uint64_t b, uint64_t c, uint64_t runner)
{
    (void)a; (void)b; (void)c;
    TileRunner* r = (TileRunner*)(uintptr_t)runner;
    TlsSetValue(g_tileRunnerTls, r);
    if (!g_tileStop && g_world.levels == g_coarse.level) CoarseOne(r, 1);
    else InterlockedDecrement(&g_coarse.pending);
    TlsSetValue(g_tileRunnerTls, NULL);
    MemoryBarrier();
    InterlockedExchange(&g_coarse.busy[r->id], 0);
    InterlockedDecrement(&g_coarse.inflight);
    InterlockedIncrement(&g_tileFinished);
    return 0;
}

/* Main thread, each frame of the section: one job per idle runner while free slots and unclaimed tiles remain. */
static void CoarseFeed(void)
{
    typedef uint64_t (*SubmitFn)(void* fn, uint64_t* data, const char* name, uint32_t queue, uint32_t priority, uint32_t count, uint64_t zero);
    static const char name[] = "hellforge coarse terrain tile";
    if (g_tileStop || g_coarse.submit == 0) return;
    int free = 0;
    for (int i = 0; i < g_coarse.slotCount; i++) if (g_coarse.slots[i].state == STAMP_SLOT_FREE) free++;
    for (int id = 1; id <= g_coarse.started; id++)
    {
        LONG pending = g_coarse.pending; /* submitted, slot not yet claimed */
        if (free - pending <= 0 || g_coarse.nextTile + pending >= g_coarse.count) return;
        if (g_coarse.busy[id]) continue;
        g_coarse.busy[id] = 1;
        InterlockedIncrement(&g_coarse.inflight); InterlockedIncrement(&g_coarse.pending);
        g_coarse.jobs++;
        ((SubmitFn)(uintptr_t)g_coarse.submit)((void*)CoarseJob, &g_coarse.element[id], name, 1, 3, 1, 0);
    }
}

/* The runners' state, once a section when no tile finishes for 10 s (diagnostics for a stall). */
static void TileRunnersStallNote(const char* section, int started)
{
    static const char* const kAt[] = {"idle", "build", "Voronoi", "op", "waiting for the sort lock", "waiting for the image lock", "move", "prepare",
        "waiting for a slot", "op 0x2B", "zones"};
    char text[1400]; int len = 0;
    for (int id = 0; id <= started && id < TILE_RUNNERS_MAX && len < (int)sizeof text - 80; id++)
    {
        TileRunner* r = g_tileRunners[id];
        if (r == NULL) continue;
        LONG w = r->where;
        len += snprintf(text + len, sizeof text - len, "%s%d: tile %ld op 0x%02lX %s", len ? "; " : "", id, (long)r->tile, (long)r->op,
            w >= 0 && w < (LONG)(sizeof kAt / sizeof kAt[0]) ? kAt[w] : "?");
    }
    Note("difficulty: %s: no tile finished for 10 s; the runners: %s (%ld of %d jobs returned)", section, text, (long)g_tileFinished, started);
}

/* Once the template is made (TerrainStep's first call): the parallel section, or 0 for the serial path. */
static int CoarseStart(uint64_t records, uint32_t end, uint64_t context, uint64_t g, uint64_t h, uintptr_t gen)
{
    g_coarse.on = 0;
    int R = TileThreads();
    uintptr_t settings = 0, lg = 0, spacingAt = 0, P = 0;
    if (R < 2 || !TileBuildsOn() || !TileDispatchInstall() || SafeRead(context + 8, &settings, 8) || SafeRead(context + 0x18, &lg, 8) || lg == 0 ||
        settings != lg + 0x2D20 || SafeRead(lg + 0x2D0, &spacingAt, 8) || spacingAt == 0 || SafeRead(spacingAt + 0x68, &spacingAt, 8) || spacingAt == 0 ||
        SafeRead(lg + 0x2D7C, &g_coarse.disk, 4) || SafeRead(spacingAt, &g_coarse.spacing, 4) || SafeRead(settings, g_coarse.buildS, sizeof g_coarse.buildS) ||
        SafeRead(context, g_coarse.ctx, sizeof g_coarse.ctx) || SafeRead(context + 0x20, &P, 8))
        return 0;
    g_coarse.count = 0;
    for (int t = 0; t < g_terrain.tiles && g_coarse.count < 1024; t++) if (TerrainTileWanted(t)) g_coarse.list[g_coarse.count++] = t;
    int slots = R + 2 < COARSE_SLOTS_MAX ? R + 2 : COARSE_SLOTS_MAX, have = 0;
    for (; have < slots; have++)
    {
        if (g_coarse.blocks[have] == NULL) g_coarse.blocks[have] = (uint8_t*)VirtualAlloc(NULL, GEN_BLOCK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (g_coarse.blocks[have] == NULL) break;
        g_coarse.slots[have].T = g_coarse.blocks[have]; g_coarse.slots[have].state = STAMP_SLOT_FREE;
    }
    if (have < 2) return 0;
    g_coarse.slotCount = have;
    for (int k = 0; k < g_coarse.count; k++) g_coarse.slotOf[k] = -1;
    g_coarse.nextTile = 0;
    g_coarse.records = records; g_coarse.end = end; g_coarse.context = context; g_coarse.g = g; g_coarse.h = h; g_coarse.gen = gen; g_coarse.P = P;
    g_coarse.draw = (int)g_terrain.draw;
    g_coarse.lg = lg; g_coarse.spacingAt = spacingAt;
    TerrainTileZonesPrepare(gen);
    /* the section holds the marker and the spacing; the runners' build settings are the level's as they are then */
    *(volatile uint32_t*)(lg + 0x2D7C) = g_coarse.disk ^ 1u;
    *(volatile float*)spacingAt = 16.0f;
    g_tileSectionHeld = 1;
    { uint32_t flipped = g_coarse.disk ^ 1u; memcpy(g_coarse.buildS + 0x5C, &flipped, 4); }
    g_coarse.level = g_world.levels;
    g_tileStop = 0; g_tileFinished = 0; g_tileWork = NULL;
    g_coarse.started = 0; g_coarse.inflight = 0; g_coarse.pending = 0; g_coarse.jobs = 0; g_coarse.selfBuilt = 0; g_coarse.queueWaits = 0;
    g_coarse.tileTrisMax = 0; g_coarse.tileTrisSum = 0; g_coarse.frameTrisMax = 0; g_coarse.tilesCounted = 0; g_coarse.submit = 0;
    uint64_t api = 0;
    if (!SafeRead(g_world.game + 0x33263E8u, &api, 8) && api != 0 && !SafeRead(api + 0x88, &g_coarse.submit, 8) && g_coarse.submit != 0)
        for (int id = 1; id <= R - 1 && id < TILE_RUNNERS_MAX; id++)
        {
            TileRunner* r = TileRunnerMake(id);
            if (r == NULL) break;
            g_coarse.element[id] = (uint64_t)(uintptr_t)r; g_coarse.busy[id] = 0;
            g_coarse.started = id;
        }
    if (g_coarse.started == 0)
    {
        *(volatile uint32_t*)(lg + 0x2D7C) = g_coarse.disk; *(volatile float*)spacingAt = g_coarse.spacing; g_tileSectionHeld = 0;
        return 0;
    }
    g_coarse.on = 1; g_coarse.level = g_world.levels; g_coarse.waitSince = 0; g_coarse.stallNoted = 0;
    Note("difficulty: terrain: %d tiles on %d runners (%d slots), drawn in tile order", g_coarse.count, g_coarse.started, g_coarse.slotCount);
    return 1;
}

static void CoarseEnd(void)
{
    g_tileStop = 1;
    ULONGLONG since = GetTickCount64();
    while (g_coarse.inflight > 0)
    {
        if (GetTickCount64() - since > 30000)
        {
            Note("difficulty: terrain: %ld coarse tile jobs did not return within 30 s; no runners for the rest of the session", (long)g_coarse.inflight);
            g_tileJobsBroken = 1;
            break;
        }
        Sleep(1);
    }
    Note("difficulty: terrain: the section's %ld tile jobs on %d runners returned; %ld tiles built on the dispatcher's thread; %ld frames ended on a full generation queue", g_coarse.jobs, g_coarse.started, g_coarse.selfBuilt, g_coarse.queueWaits);
    Note("difficulty: terrain: gui triangles: a tile's draw at most %llu, mean %llu; one draw call at most %llu", (unsigned long long)g_coarse.tileTrisMax,
        (unsigned long long)(g_coarse.tilesCounted ? g_coarse.tileTrisSum / (uint64_t)g_coarse.tilesCounted : 0), (unsigned long long)g_coarse.frameTrisMax);
    *(volatile uint32_t*)(g_coarse.lg + 0x2D7C) = g_coarse.disk;
    *(volatile float*)g_coarse.spacingAt = g_coarse.spacing;
    g_tileSectionHeld = 0;
    if (!g_tileJobsBroken)
    {
        for (int i = 0; i < COARSE_SLOTS_MAX; i++) if (g_coarse.blocks[i] != NULL) { VirtualFree(g_coarse.blocks[i], 0, MEM_RELEASE); g_coarse.blocks[i] = NULL; }
        TileRunnersRelease();
    }
    g_coarse.on = 0;
}

/* One frame of the parallel section (the dispatcher's thread): the built tiles drawn in tile order, 1 while tiles
 * remain. Each RenderHeightmapVoronoi call (0xF499B0) appends one entry to the generation's queue [ctx+0x50] = G+0xAC570
 * (0xF4A195..0xF4A254: count +0x10, 16 entries of 0x1D0 from +0x18, no capacity check), and only the render callback
 * drains it, once a frame (0xAB7D54 -> 0xAB0590). A 17th entry lands on G+0xAE288, the current level, with the world
 * ([ctx+0x40]) as its first qword (t77h/t77i/t77j: the objective selection then read [world+0x2D0] = 0 and faulted at
 * game+0x1757304). And each entry is one render_world (0xAB07E1 -> [api+0x10]+0x28) of the generator's world through the
 * camera [R], which draws every gui triangle put there this frame: N draw calls in a frame drew each frame's additive
 * heights N times (t77k..t77m: the coarse terrain an egg-crate up to the u16's 500 m, base 265-620 m against 80-125 m
 * serial). So a draw call is made only with the queue empty (one entry, one render a frame, as the serial path: one
 * call a tile a frame), else the level waits for the next frame. Runner jobs build meanwhile; the
 * dispatcher's thread builds a tile itself only when no job is in flight. */
static uint32_t NearQueued(uint64_t context);
static int CoarseDraw(uint64_t context, uint64_t g, uint64_t h, uintptr_t gen, LARGE_INTEGER t0, LARGE_INTEGER hz)
{
    TileRunner* mine = TileRunnerMake(0);
    for (;;)
    {
        CoarseFeed();
        if (g_terrain.next >= g_coarse.count) return 0;
        LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
        if ((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart > TERRAIN_BUDGET_MS) return 1;
        LONG at = g_coarse.slotOf[g_terrain.next];
        if (at < 0 || g_coarse.slots[at].state != STAMP_SLOT_BUILT) /* not built yet */
        {
            if (mine != NULL && g_coarse.inflight == 0 && g_coarse.nextTile < g_coarse.count)
            {
                TlsSetValue(g_tileRunnerTls, mine);
                int built = CoarseOne(mine, 0);
                TlsSetValue(g_tileRunnerTls, NULL);
                if (built) { g_coarse.selfBuilt++; g_coarse.waitSince = 0; continue; }
            }
            ULONGLONG now = GetTickCount64();
            if (g_coarse.waitSince == 0) g_coarse.waitSince = now;
            else if (now - g_coarse.waitSince > 10000 && !g_coarse.stallNoted) { g_coarse.stallNoted = 1; TileRunnersStallNote("terrain", g_coarse.started); }
            return 1;
        }
        uint32_t queued = NearQueued(context);
        if (queued != 0) { g_coarse.queueWaits++; return 1; } /* one draw call a frame: see above */
        g_coarse.waitSince = 0;
        CoarseSlot* s = &g_coarse.slots[at];
        if (!s->ok)
        {
            g_terrain.failed++; g_terrain.buildMs += s->buildMs;
            InterlockedExchange(&g_coarse.slotOf[g_terrain.next], -1); InterlockedExchange(&s->state, STAMP_SLOT_FREE);
            g_terrain.next++;
            continue;
        }
        LARGE_INTEGER d0, d1;
        QueryPerformanceCounter(&d0);
        volatile uint32_t index = 0;
        volatile int calls = 0;
        int clip = TerrainClipBegin(s->t, gen);
        uint64_t trisBefore = g_clip.tris;
        LONG fault = WorldTileGuarded(s->record, context, &index, &s->phase, g, h, 1, &calls);
        if (clip) TerrainClipEnd();
        { uint64_t d = g_clip.tris - trisBefore; s->tris += d; if (d > g_coarse.frameTrisMax) g_coarse.frameTrisMax = d; }
        QueryPerformanceCounter(&d1);
        g_terrain.drawMs += (double)(d1.QuadPart - d0.QuadPart) * 1000.0 / (double)hz.QuadPart;
        s->drawCalls++;
        int doneTile = 0;
        if (fault || (index == 0 && s->drawCalls >= TERRAIN_DRAW_CALLS))
        {
            Note("difficulty: terrain: tile %d's draw %s", s->t, fault ? "faulted" : "did not finish");
            g_terrain.failed++; doneTile = 1;
            if (fault) { g_terrain.buildMs += s->buildMs; InterlockedExchange(&s->state, STAMP_SLOT_FREE); g_terrain.next = g_coarse.count; return -1; }
        }
        else if (index != 0)
        {
            WorldArenaReset(context); g_terrain.drawn++; doneTile = 1;
            g_coarse.tileTrisSum += s->tris; g_coarse.tilesCounted++; if (s->tris > g_coarse.tileTrisMax) g_coarse.tileTrisMax = s->tris;
        }
        if (doneTile)
        {
            g_terrain.buildMs += s->buildMs;
            InterlockedExchange(&g_coarse.slotOf[g_terrain.next], -1); InterlockedExchange(&s->state, STAMP_SLOT_FREE);
            g_terrain.next++;
        }
    }
}

static int MacroStep(uint64_t context); /* difficulty_world_macro.c: the km-scale relief, drawn once the tiles are */
/* The dispatcher hook (difficulty_world.c's WorldDispatch) at the level's SmoothHeightmap: 1 while the level waits. */
static int TerrainStep(uint64_t records, uint32_t end, uint32_t at, uint64_t context, uint64_t g, uint64_t h)
{
    if (g_world.current != 8.0f || g_worldLevel == 0 || g_rec25 == 0 || !TerrainOn()) return 0;
    uintptr_t gen = 0;
    if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0) return 0;
    if (g_coarse.on && g_coarse.level != g_world.levels) CoarseEnd(); /* a level that ended inside its section */
    if (g_terrain.level != g_world.levels)
    {
        g_terrain.level = g_world.levels;
        g_terrain.state = 2;
        uint32_t size = 0;
        uintptr_t settings = 0;
        if (SafeRead(context + 8, &settings, 8) || settings == 0 || SafeRead(settings + 0x2C, &size, 4) || size != 16384u) return 0;
        uint32_t op = 0, i = at;
        while (i > 0 && !SafeRead(records + (uint64_t)(i - 1) * 0x19B0u, &op, 4) && op != 0x3C) i--;
        if (i == 0 || op != 0x3C) { Note("difficulty: terrain: no RenderHeightmapVoronoi before SmoothHeightmap; the level keeps its own terrain"); return 0; }
        g_terrain.draw = i - 1;
        if (!TerrainTemplate(gen)) { Note("difficulty: terrain: the generator block did not copy; the level keeps its own terrain"); return 0; }
        g_terrain.columns = (int)((size + TERRAIN_TILE_CORE - 1) / TERRAIN_TILE_CORE);
        g_terrain.tiles = g_terrain.columns * g_terrain.columns;
        g_terrain.next = 0; g_terrain.drawn = 0; g_terrain.failed = 0; g_terrain.building = 0;
        g_terrain.buildMs = 0; g_terrain.drawMs = 0; g_terrain.frames = 0;
        g_terrain.started = GetTickCount64();
        g_terrain.state = 1;
        CoarseStart(records, end, context, g, h, gen);
        Note("difficulty: terrain: building the map at vanilla density, %d m tiles (%d m drawn; %d x %d) into the level's merge (record %u); the level waits",
            TERRAIN_TILE_EXTENT, TERRAIN_TILE_CORE, g_terrain.columns, g_terrain.columns, g_terrain.draw);
    }
    if (g_terrain.state != 1) return g_terrain.state == 2 ? MacroStep(context) : 0;
    g_terrain.frames++;
    LARGE_INTEGER hz, t0, t1;
    QueryPerformanceFrequency(&hz);
    QueryPerformanceCounter(&t0);
    if (g_coarse.on)
    {
        int more = CoarseDraw(context, g, h, gen, t0, hz);
        if (more > 0) return 1;
        CoarseEnd();
        g_terrain.state = 2;
        Note("difficulty: terrain: %d tiles drawn, %d failed, in %.1f s (%u frames; building %.1f s summed over the runners, drawing %.1f s)", g_terrain.drawn,
            g_terrain.failed, (double)(GetTickCount64() - g_terrain.started) / 1000.0, g_terrain.frames, g_terrain.buildMs / 1000.0, g_terrain.drawMs / 1000.0);
        WorldMemoryNote("terrain tiles drawn", 0xFFFFFFFFu, 0, 1);
        return more < 0 ? 0 : MacroStep(context);
    }
    for (;;)
    {
        QueryPerformanceCounter(&t1);
        double used = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart;
        if (used > TERRAIN_BUDGET_MS) return 1;
        if (!g_terrain.building)
        {
            while (g_terrain.next < g_terrain.tiles && !TerrainTileWanted(g_terrain.next)) g_terrain.next++;
            if (g_terrain.next >= g_terrain.tiles)
            {
                g_terrain.state = 2;
                Note("difficulty: terrain: %d tiles drawn, %d failed, in %.1f s (%u frames; building %.1f s, drawing %.1f s)", g_terrain.drawn,
                    g_terrain.failed, (double)(GetTickCount64() - g_terrain.started) / 1000.0, g_terrain.frames, g_terrain.buildMs / 1000.0,
                    g_terrain.drawMs / 1000.0);
                WorldMemoryNote("terrain tiles drawn", 0xFFFFFFFFu, 0, 1);
                return MacroStep(context);
            }
            LARGE_INTEGER b0, b1;
            QueryPerformanceCounter(&b0);
            int built = TerrainBuild(g_terrain.next, records, end, context, g, h, gen, 1) && TerrainDrawPrepare(records, gen);
            QueryPerformanceCounter(&b1);
            g_terrain.buildMs += (double)(b1.QuadPart - b0.QuadPart) * 1000.0 / (double)hz.QuadPart;
            if (!built) { g_terrain.failed++; g_terrain.next++; continue; }
            g_terrain.building = 1;
        }
        if (NearQueued(context) != 0) return 1; /* CoarseDraw: one draw call a frame */
        LARGE_INTEGER d0, d1;
        QueryPerformanceCounter(&d0);
        volatile uint32_t index = 0;
        volatile int calls = 0;
        int clip = TerrainClipBegin(g_terrain.next, gen);
        LONG fault = WorldTileGuarded(g_terrain.record, context, &index, &g_terrain.phase, g, h, 1, &calls);
        if (clip) TerrainClipEnd();
        QueryPerformanceCounter(&d1);
        g_terrain.drawMs += (double)(d1.QuadPart - d0.QuadPart) * 1000.0 / (double)hz.QuadPart;
        g_terrain.drawCalls++;
        if (fault || (index == 0 && g_terrain.drawCalls >= TERRAIN_DRAW_CALLS))
        {
            Note("difficulty: terrain: tile %d's draw %s", g_terrain.next, fault ? "faulted" : "did not finish");
            g_terrain.failed++; g_terrain.building = 0; g_terrain.next++;
            if (fault) { g_terrain.state = 2; return 0; } /* the merge's state is unknown after a fault: stop drawing */
            continue;
        }
        if (index != 0) { WorldArenaReset(context); g_terrain.drawn++; g_terrain.building = 0; g_terrain.next++; }
    }
}

/* ---- Near-player terrain: a 2048 m tile at vanilla's 0.5 m per sample around the drop point --------------------------
 * The level's terrain covers 16 km with one 4096^2 heightmap (4 m a sample): good from afar, coarse underfoot. Around the
 * drop point a second terrain, 2048 m on a side, gets its own 4096^2 height and material maps (vanilla's density),
 * rendered by the level's own terrain merge a second time:
 *
 * 1. At the level's CleanupTerrains (0x65, after its RenderFinalize 0x64 wrote the two named globals and before 0x66
 *    reads them back and tears the merge down) the centre is the squad's drop point on the war board (else a point
 *    chosen from the level's capture (lg+0xC0, lg = G+0x8758A8
 *    the merge's state; float metres, row 0 at +y, and the water levels, 0x0A), and the job queue [ctx+0x50] is waited empty.
 * 2. Two textures are created as the render config's generated_heightmap (0x2A9E3232) and generated_materials
 *    (0x95E7E3AC) are (format +0x8, size +0x14/+0x18, mips +0x20, flags +0x60/+0x63/+0x64 of the config objects; render
 *    API +0x170) and published under those names (+0x210): a published name takes the config's place for the merge's
 *    GPU passes (run pt1).
 * 3. Both merge cameras (R+0 node [R+8], R+0x10 node [R+0x18], R = [G+0]; orthographic, straight down from z 501) get
 *    the tile's extents (+-1024, [api+0x20]+0x98) and centre ([api+0x20]+0x20, then [api+8]+0x100), as SetupTerrainMerge
 *    aims them for the world (0xF487B0 / 0xF48990). Jobs that carry their own camera position (0x41, 0x0D: entry +0x24)
 *    get the centre added (WorldCamSetPosition, caller game 0xAB0725).
 * 4. The passes that draw the merge's gui (R+0x110) draw in pixels: position x samples / world size, y up as the world
 *    (0x41's tile jobs: tile (i, j) at the camera position of tile column i, row j, gui offset (-i, -j) x samples, scale
 *    T; applied as pixel x scale + offset). For the tile: scale world / 2048, offset -(c + world / 2 - 1024) x samples /
 *    2048 per axis, composed with a job's own (scale s s0, offset o s0 + o0). The job consumer (game 0xAB0590) runs
 *    each job's generator through [api+0x10]+0x28; a wrapper on that slot sets the gui transform first (for jobs on the
 *    merge cameras; others get the identity, as in the level's run).
 * 5. The level's merge records run again on copies, as the level ran them: 0x39 clear, 0x3C its Voronoi cells (+0x108
 *    progress zeroed), the vanilla-density tiles that touch the near square (TerrainBuild, drawn as at 0x40), 0x40
 *    smoothing (not 0x94, 0x3E or 0x3F: they squeeze the level into the tile, NearPlan; the conform holds the level's
 *    pads and flatten zones instead), 0x3D zone heights (materials repeating every 2048 m, as on a vanilla map), 0x0D route
 *    segments, 0x63 stamp heights, 0x42, 0x41 materials, 0x64 finalize.
 * 6. Once the queue is empty, the tile's heightmap is read back (+0x1A8/+0x1D0/+0x1D8: the GPU work before it is done, and
 *    the heights are logged), the cameras and gui go back as the level had them and both names are unpublished (0x66
 *    then reads the level's own).
 * 7. After the level's SpawnTerrainFinalize (0x6B) InitSpawnTerrain (0xF44DC0) runs again with the spawn slot
 *    ([api+8]+0x198 -> exe 0x1AABE0) wrapped: the pose moves to the tile's centre, the size is 2048 - 0.5, and for that
 *    call the render config's slots for the two names (exe 0xA6E30 on [[world+0x2301F8]]+0x1600, the spawn's only
 *    lookup) hold the tile's textures. Then the ready wait, SpawnTerrainWork and finalize, with the collision moved to the
 *    tile (finalize builds the body from the terrain object alone, centred on the world origin).
 * Off with HELLFORGE_XL_NEAR=0. */
/* Where the tile meets the coarse terrain. The coarse terrain hides 2 x 2 of its level-4 quadtree nodes under the tile
 * (NearHideCoarse; whole level-4 nodes, as a partly hidden node is drawn whole from far enough), NEAR_HIDE square,
 * 512 coarse samples (d_c = 16383.5 / 4095 m apart) from sample to sample. Neither terrain's own edge may be in view:
 * - the terrain pixel shader (p_69cdb47b) takes its normal from 4 unclamped `ld` reads at +-1 texel, so the outermost
 *   texel of any terrain reads 0 (-500 m) beyond its map and shades dark (s70q: a 0.5 m dark line along a tile whose
 *   edge was the hidden square's);
 * - the coarse terrain's free edge around the hidden square, where the two meshes' LOD lattices differ (s70p/s70q
 *   data, coarse 4..16 m against the tile's 2.25..18 m: up to 0.5..3 m apart; with both maps at one mip, NearOneMip).
 * So the tile is NEAR_SIDE (texels; NEAR_SPAN sample to sample) with a band past the hidden square that follows the
 * coarse terrain exactly for NEAR_BAND_FLAT, sinks NEAR_BAND_SINK under it, and then dives NEAR_BAND_DEPTH under it by NEAR_BAND_END (its own edge,
 * 128 m out, lies under the coarse terrain), and the coarse terrain's samples on the hidden square's edge (its free
 * edge's vertices, at every LOD) are drawn NEAR_RING_DIP lower in the level's own heightmap (NearRingStep, before the
 * level's finalize): its free edge lies under the tile, and the visible switch from tile to coarse is the two surfaces
 * crossing a few metres out. Offline check (scratchpad ringsim.py, both maps, s70u): with the ring 0.25 m down no point
 * where the coarse edge stands above the tile up to 16 m coarse spacing (1.8 km), the visible surface within 0.22 m
 * below the coarse terrain's own LOD surface. The dip is small because the coarse terrain's normals (central
 * differences over +-1 sample, 4 m) see it: 2 m tilted them ~14 deg over the 4 m past the tile's flat strip, a lit
 * stripe along every edge (audit of s70u); 0.25 m tilts them ~1.8 deg. */
#define NEAR_DC (16383.5f / 4095.0f)
#define NEAR_HIDE (512.0f * NEAR_DC) /* 2048.44 m: the hidden coarse nodes, sample to sample */
#define NEAR_SIDE 2304.0f /* the tile's texels' span (the replay's square), 0.5625 m a texel */
#define NEAR_SPAN (NEAR_SIDE * 4095.0f / 4096.0f) /* the tile, sample to sample (its terrain size) */
#define NEAR_TEXEL (NEAR_SIDE / 4096.0f)
/* The edge's tunables, read again at every level's first op from HELLFORGE_XL_PARAMS (a text file, "name value" lines,
 * # comments; research: a leave and a new mission try new values without a restart). Defaults:
 * - ring_dip 0.25: ringsim (s70u): no exposed coarse edge up to 16 m spacing; the coarse normals tilt ~1.8 deg next to
 *   it (2 m: ~14 deg, a lit stripe);
 * - band_flat 4: m past the hidden square where the tile follows the coarse terrain exactly; band_sink 0.4 / band_ease
 *   3: it then sinks this far under it, gently (from a flat start), over this many metres, before the dive; band_end 16
 *   / band_depth 16: m past it where the tile is band_depth under it (ease-out after the sink);
 * - fine 9: the conform's per-texel strip around the edge (past the sink by two texels and more);
 * - mat_band 48 / mat_wobble 20 / mat_edge 2: the material band (NearMatBand);
 * - foot_band 0.5 / foot_band_min 16: a stamp footprint's band, x its radius, metres at least (NearPadFootprint). */
static struct
{
    LONG level;
    float ringDip, bandFlat, bandSink, bandEase, bandEnd, bandDepth, fine, matBand, matWobble, matEdge, footBand, footBandMin;
} g_xp = {-1, 0.25f, 4.0f, 0.4f, 3.0f, 16.0f, 16.0f, 9.0f, 48.0f, 20.0f, 2.0f, 0.5f, 16.0f};
#define NEAR_RING_DIP (g_xp.ringDip)
#define NEAR_BAND_FLAT (g_xp.bandFlat)
#define NEAR_BAND_SINK (g_xp.bandSink)
#define NEAR_BAND_EASE (g_xp.bandEase)
#define NEAR_BAND_END (g_xp.bandEnd)
#define NEAR_BAND_DEPTH (g_xp.bandDepth)

static void NearParamsLoad(void)
{
    if (g_xp.level == g_world.levels) return;
    g_xp.level = g_world.levels;
    char path[MAX_PATH] = "";
    if (GetEnvironmentVariableA("HELLFORGE_XL_PARAMS", path, sizeof path) == 0) return;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { Note("difficulty: near terrain: parameters: %s not found; the defaults stand", path); return; }
    char text[4096]; DWORD got = 0;
    ReadFile(f, text, sizeof text - 1, &got, NULL); CloseHandle(f); text[got] = 0;
    static const struct { const char* name; size_t at; } kParams[] = {
        {"ring_dip", offsetof(__typeof__(g_xp), ringDip)}, {"band_flat", offsetof(__typeof__(g_xp), bandFlat)},
        {"band_sink", offsetof(__typeof__(g_xp), bandSink)}, {"band_ease", offsetof(__typeof__(g_xp), bandEase)},
        {"band_end", offsetof(__typeof__(g_xp), bandEnd)}, {"band_depth", offsetof(__typeof__(g_xp), bandDepth)},
        {"fine", offsetof(__typeof__(g_xp), fine)}, {"mat_band", offsetof(__typeof__(g_xp), matBand)},
        {"mat_wobble", offsetof(__typeof__(g_xp), matWobble)}, {"mat_edge", offsetof(__typeof__(g_xp), matEdge)},
        {"foot_band", offsetof(__typeof__(g_xp), footBand)}, {"foot_band_min", offsetof(__typeof__(g_xp), footBandMin)}};
    char applied[512] = ""; size_t used = 0;
    for (char* line = strtok(text, "\r\n"); line != NULL; line = strtok(NULL, "\r\n"))
    {
        char name[32]; float v;
        if (line[0] == '#' || sscanf(line, " %31[a-z_] %*[= ] %f", name, &v) != 2) continue;
        for (size_t k = 0; k < sizeof kParams / sizeof kParams[0]; k++)
            if (strcmp(name, kParams[k].name) == 0 && v == v && v > -1000.0f && v < 1000.0f)
            {
                *(float*)((uint8_t*)&g_xp + kParams[k].at) = v;
                if (used < sizeof applied - 40) used += (size_t)snprintf(applied + used, sizeof applied - used, " %s=%g", name, (double)v);
            }
    }
    if (g_xp.bandEnd < g_xp.bandFlat + g_xp.bandEase + 1.0f) g_xp.bandEnd = g_xp.bandFlat + g_xp.bandEase + 1.0f;
    Note("difficulty: near terrain: parameters from %s:%s", path, used ? applied : " none (the defaults stand)");
}
#define NEAR_STEPS 48

typedef uint64_t (*NearSpawnFn)(void* world, float* pose, uint32_t a, uint32_t type, uint32_t name, uint64_t resource, uint32_t hash,
    float* size, float* range, uint8_t flag);
typedef uint64_t (*NearRunFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef void (*NearGuiFn)(uint64_t gui, float x, float y);
typedef void (*NearExtentsFn)(uint64_t camera, float left, float right, float bottom, float top, uint32_t flag);
typedef void (*NearUnitFn)(uint64_t world, uint32_t node);
typedef uint64_t (*NearFindFn)(uint32_t name);
typedef uint64_t (*NearCreateFn)(uint32_t w, uint32_t h, uint32_t mips, uint32_t format, uint32_t id, uint32_t flags);
typedef void (*NearPublishFn)(uint32_t name, uint64_t resource);
typedef void (*NearNameFn)(uint32_t name);
typedef void (*NearReleaseFn)(uint64_t resource);
typedef uint32_t (*NearReadbackFn)(uint64_t resource);
typedef char (*NearReadyFn)(uint32_t id);
typedef const uint8_t* (*NearDescFn)(uint8_t* out, uint32_t id);
typedef uint64_t* (*NearSlotFn)(uint64_t table, uint32_t* key);

static const uint32_t kNearNames[2] = {0x2A9E3232u, 0x95E7E3ACu}; /* generated_heightmap, generated_materials */
/* painted_roads_height: the render config's alias of generated_heightmap (type "alias", aliased_resource
 * "generated_heightmap"; the only alias of either map). The config parser stores the aliased resource's pointer under
 * the alias's own name (exe 0x52D214-0x52D353), and a published name replaces only its own key (0x52C690), so the alias
 * keeps the level's texture unless it is published too. SmoothHeightmap's layers (0x40: road_height clears it,
 * road_blend_height writes it as a UAV, copy:OUTPUT_16UNORM) write through it: without this the tile's replay wrote the
 * tile's smoothed heights into mip 0 of the level's own heightmap (run v11: the level's mip 0 = what its CPU copy and
 * collision were made from, its mips 1-4 = the level's real terrain, mean |d| 57 m). Name = murmur64(name) >> 32. */
static const uint32_t kNearRoadAlias = 0xBF45B677u;
enum { NEAR_IDLE, NEAR_START, NEAR_STEP, NEAR_DRAIN, NEAR_READBACK, NEAR_SPAWN, NEAR_READY, NEAR_IN, NEAR_FAILED, NEAR_CAL, NEAR_CONFORM };
enum { NEAR_RECORD, NEAR_TILE };

static NearSpawnFn g_nearSpawnOriginal;
static volatile DWORD g_nearSpawnThread; /* the thread spawning the tile (InitSpawnTerrain called by TerrainNearStep) */
static uint64_t* g_nearSlots[2];         /* the render config's slots for the two names (exe 0xA6E30), checked before the spawn */
static NearRunFn g_nearRunOriginal;
static volatile LONG g_nearActive; /* the replay's jobs are queued: the wrappers apply the tile's transform */
static volatile LONG g_moveBuilding; /* a tile is being built in mission (difficulty_world_move.c) */
/* A tile built in mission draws its stamp terrains away from every terrain (difficulty_world_move.c): the stamp units
 * stand at their poses + this offset, and the merge cameras move by it for the replayed 0x63 alone. 0, 0 otherwise. */
static float g_nearStampOffset[2];
static void MoveLevelGone(void);      /* difficulty_world_move.c: a new level starts, what the last one built in mission goes */
static void MoveInstall(uintptr_t game);
static struct {
    LONG level; int state;
    /* the level's intermediate captures as its own 0x3E read them (8 descriptors {w, h, format, ?, data}) */
    LONG capLevel; uint8_t caps[0xC0]; uint8_t* capData[8]; size_t capBytes[8];
    float centre[2];
    uint64_t R, gui, cam[2], world, settings, lg; uint32_t node[2];
    float scale, offset[2], world_size, samples;
    uint64_t tex[2], config[2], oldTex[2];
    uint32_t stepRecord[NEAR_STEPS]; int stepTile[NEAR_STEPS]; uint8_t stepKind[NEAR_STEPS], stepOp[NEAR_STEPS];
    int steps, step, building, tilesDrawn, tilesFailed;
    uint8_t one[0x19B0]; uint8_t phase; int calls; uint32_t frames, waits;
    int loggedStep; /* a mid-mission build: the last step whose start was noted, + 1 */
    int stampLeave; /* the replayed 0x63 drew at the stamp offset: the cameras go back once its job is drained */
    uint32_t stampWaits;
    int matLeave; /* the replayed 0x41 finished: its cameras go back once its jobs are drained */
    uint32_t readId; float* heights; uint32_t hw, hh;
    uint8_t one69[0x19B0], one6B[0x19B0]; uint32_t at69, at6B;
    uint8_t keep69[0x19B0], keep6B[0x19B0]; /* the two records as the level had them, for the tiles built later (difficulty_world_move.c) */
    uint64_t records; uint32_t end, at65;    /* the level's records, their count and the 0x65 the replay ran at */
    uint64_t handle, handleCell;
    float* global; uint32_t columns, rows;
    ULONGLONG started;
    int ready; /* the drop point is chosen */
    uint32_t polls;
} g_near = {.level = -1, .capLevel = -1};

static uintptr_t NearApi(uint32_t table, uint32_t slot)
{
    uintptr_t api = 0, t = 0, f = 0;
    if (SafeRead(g_world.game + 0x3326308u, &api, 8) || api == 0 || SafeRead(api + table, &t, 8) || t == 0 || SafeRead(t + slot, &f, 8)) return 0;
    return f;
}

/* ---- The level's heightmap at one mip (extra large levels) -------------------------------------------------------------
 * The terrain VS (material 0x972691f0d6cab16b) reads each vertex's height with sample_l at lod 2 sqrt(clamp((d - 15) /
 * 100, 0, 4)): from mip 1 at 40 m to mip 4 past 415 m, box-smoothed over 4 x 2^k m on the coarse terrain and 0.56 x 2^k m
 * on the tile, so the two disagree at their shared edge by up to 8 m at a distance (s70n). With both at one mip every
 * lookup reads the exact sample (a 1-mip SRV clamps the lod; normals are built from mip 0 in the PS anyway; the
 * finalize's generate_mips skips mips a texture does not have, exe 0x397859). The render config makes
 * generated_heightmap once (5 mips, shared by every level), so an extra large level gets its own 1-mip copy, published
 * under its name and its alias painted_roads_height before the level first writes it (op 0x40; published at 0x38): every
 * render-config layer and generator resolves names through the published overlay (0x52C690). The terrain spawn (exe
 * 0x1AABE0) reads the config table (0xA6E30 on renderer+0x1600) directly, so its slot holds the copy for the level's own
 * spawn call (op 0x69). Unpublished when the next level starts (a leftover publish would be the next level's map). */
static struct { LONG level; uint64_t tex, config; uint64_t* slot; void* original; volatile LONG armed; DWORD thread; int spawned; } g_oneMip = {.level = -1};
/* The coarse terrain's interpolated material map (NearCoarseMatBuild, below): its texture, pixels, the spawn's slot. */
static struct { LONG level; uint64_t tex; uint32_t* pixels; ULONGLONG made; int holds, served, scatter; uint64_t* slot; } g_coarseMat = {.level = -1};

static void NearOneMipPublish(void)
{
    if (g_oneMip.tex == 0 || g_oneMip.level != g_world.levels) return;
    NearPublishFn publish = (NearPublishFn)NearApi(0x10, 0x210);
    if (publish == NULL) return;
    publish(0x2A9E3232u, g_oneMip.tex); publish(0xBF45B677u, g_oneMip.tex); /* generated_heightmap, painted_roads_height */
}

/* A level starts (any): the previous extra large level's copy is unpublished (released at the next copy's creation). */
static void NearOneMipDrop(void)
{
    if (g_oneMip.tex == 0 || g_oneMip.level == -2) return;
    NearNameFn unpublish = (NearNameFn)NearApi(0x10, 0x218);
    if (unpublish != NULL) { unpublish(0x2A9E3232u); unpublish(0xBF45B677u); }
    g_oneMip.level = -2; /* the copy stays allocated until the next one is made: its terrain may still be drawn now */
}

/* The level's SetupTerrainMerge (0x38), before anything writes its heightmap: the copy, published. Once per level. */
static void NearOneMipLevel(void)
{
    if (g_oneMip.level == g_world.levels) return;
    NearFindFn find = (NearFindFn)NearApi(0x10, 0x158);
    NearCreateFn create = (NearCreateFn)NearApi(0x10, 0x170);
    NearReleaseFn release = (NearReleaseFn)NearApi(0x10, 0x198);
    if (g_oneMip.tex != 0 && release != NULL) { if (g_oneMip.level != -2) NearOneMipDrop(); release(g_oneMip.tex); }
    g_oneMip.tex = 0; g_oneMip.config = 0; g_oneMip.level = g_world.levels; g_oneMip.spawned = 0;
    char t[4] = "";
    if (GetEnvironmentVariableA("HELLFORGE_XL_ONEMIP", t, sizeof t) > 0 && t[0] == '0') { Note("difficulty: near terrain: the level's heightmap keeps its mips (HELLFORGE_XL_ONEMIP=0)"); return; }
    uint64_t config = find != NULL ? find(0x2A9E3232u) : 0;
    uint32_t format = 0, w = 0, hgt = 0, mips = 0; uint8_t f60 = 0, f63 = 0, f64 = 0;
    if (create == NULL || config == 0 || SafeRead(config + 0x8, &format, 4) || SafeRead(config + 0x14, &w, 4) || SafeRead(config + 0x18, &hgt, 4) ||
        SafeRead(config + 0x20, &mips, 4) || SafeRead(config + 0x60, &f60, 1) || SafeRead(config + 0x63, &f63, 1) || SafeRead(config + 0x64, &f64, 1) ||
        w != 4096 || hgt != 4096 || format != 22)
    { Note("difficulty: near terrain: the level's heightmap is not the reviewed texture (%u x %u format %u); it keeps its mips", w, hgt, format); return; }
    uint32_t flags = (f63 == 1 ? 1u : 0u) | (f60 == 1 ? 2u : 0u) | (f60 == 8 ? 4u : 0u) | (f64 == 0x20 ? 8u : 0u);
    g_oneMip.config = config; /* the render config's own (find sees the published copy from now on) */
    g_oneMip.tex = create(w, hgt, 1, format, 0x48460010u, flags);
    if (g_oneMip.tex == 0) { Note("difficulty: near terrain: the level's one-mip heightmap was not created; it keeps its mips"); return; }
    NearOneMipPublish();
    Note("difficulty: near terrain: the level's heightmap is a one-mip copy (%u x %u, format %u, flags 0x%X; the render config's has %u mips)", w, hgt, format,
        flags, mips);
}

/* The spawn slot while the level's own InitSpawnTerrain (op 0x69) runs: the config table's heightmap slot holds the copy. */
static uint64_t NearOneMipSpawn(void* world, float* pose, uint32_t a, uint32_t type, uint32_t name, uint64_t resource, uint32_t hash, float* size,
    float* range, uint8_t flag);
static void NearOneMipDisarm(void)
{
    if (!g_oneMip.armed) return;
    uintptr_t api = 0, table = 0;
    if (!SafeRead(g_world.game + 0x3326308u, &api, 8) && api != 0 && !SafeRead(api + 8, &table, 8) && table != 0)
    {
        volatile uint64_t* slot = (volatile uint64_t*)(table + 0x198);
        DWORD old = 0;
        if (*slot == (uint64_t)(uintptr_t)NearOneMipSpawn && VirtualProtect((void*)slot, 8, PAGE_READWRITE, &old))
        { *slot = (uint64_t)(uintptr_t)g_oneMip.original; VirtualProtect((void*)slot, 8, old, &old); }
    }
    g_oneMip.armed = 0; g_oneMip.slot = NULL;
}

static void NearOneMipArm(uint64_t context)
{
    if (g_oneMip.armed || g_oneMip.tex == 0 || g_oneMip.level != g_world.levels) return;
    uintptr_t api = 0, table = 0, world = 0, holder = 0, renderer = 0;
    if (SafeRead(g_world.game + 0x3326308u, &api, 8) || api == 0 || SafeRead(api + 8, &table, 8) || table == 0 || SafeRead(context + 0x40, &world, 8) ||
        world == 0 || SafeRead(world + 0x2301F8u, &holder, 8) || holder == 0 || SafeRead(holder, &renderer, 8) || renderer == 0) return;
    uint32_t key = 0x2A9E3232u;
    uint64_t* slotAt = ((NearSlotFn)((uintptr_t)GetModuleHandleW(NULL) + 0xA6E30u))(renderer + 0x1600, &key);
    uint64_t now = 0; uint8_t a = 0, b = 0;
    if (slotAt == NULL || SafeRead((uintptr_t)slotAt, &now, 8) || now == 0 || SafeRead(now + 3, &a, 1) || SafeRead(g_oneMip.tex + 3, &b, 1) || a != b)
    { Note("difficulty: near terrain: the level's heightmap slot is not the reviewed one; its terrain keeps the 5-mip map"); return; }
    /* the coarse terrain's interpolated material map (NearCoarseMatBuild) in the material slot, if that slot holds the
     * texture the conform read back */
    g_coarseMat.slot = NULL;
    if (g_coarseMat.tex != 0 && g_coarseMat.level == g_world.levels)
    {
        uint32_t mkey = 0x95E7E3ACu;
        uint64_t* mat = ((NearSlotFn)((uintptr_t)GetModuleHandleW(NULL) + 0xA6E30u))(renderer + 0x1600, &mkey);
        uint64_t held = 0;
        if (mat != NULL && !SafeRead((uintptr_t)mat, &held, 8) && held == g_near.config[1] && held != 0) g_coarseMat.slot = mat;
        else Note("difficulty: near terrain: the level's material slot holds 0x%llX, not the map read back (0x%llX); its terrain keeps the level's map",
            (unsigned long long)held, (unsigned long long)g_near.config[1]);
    }
    volatile uint64_t* slot = (volatile uint64_t*)(table + 0x198);
    DWORD old = 0;
    if (!VirtualProtect((void*)slot, 8, PAGE_READWRITE, &old)) return;
    g_oneMip.original = (void*)(uintptr_t)*slot;
    g_oneMip.slot = slotAt; g_oneMip.thread = GetCurrentThreadId(); g_oneMip.armed = 1;
    *slot = (uint64_t)(uintptr_t)NearOneMipSpawn;
    VirtualProtect((void*)slot, 8, old, &old);
}

/* Op 0x41's painters drawn in vanilla space (TerrainMat*, below): on while the merge cameras are aimed for it. */
static struct {
    volatile LONG on, patched; /* cameras in vanilla space; the pose loads point at the mod's rows (a 0x41 call runs) */
    DWORD thread;              /* the thread of that call (the noise wrapper acts on it alone) */
    int owner, waits, quiet;   /* MAT_LEVEL / MAT_TILE */
    LONG level;                /* g_world.levels when entered */
    float s;                   /* 2048 / the span drawn */
    uint64_t cam[2], world; uint32_t node[2];
    float back[2], backAt[2]; /* the cameras' half extents and centre to put back */
    uint8_t* rows;            /* the three pose rows, 16-byte aligned, within 2 GB of game.dll */
} g_mat;
enum { MAT_LEVEL = 1, MAT_TILE = 2 };

/* WorldCamSetPosition: a replayed job's own camera position (the consumer's call at game 0xAB0721) moves to the tile
 * (not for the tile's 0x41 in vanilla space: its cameras stay at the origin) */
static int TerrainNearCamShift(uint64_t camera, uintptr_t at, const float* v, float* out)
{
    if (!g_nearActive || g_mat.on || at != g_world.game + 0xAB0725u || (camera != g_near.cam[0] && camera != g_near.cam[1])) return 0;
    out[0] = v[0] + g_near.centre[0]; out[1] = v[1] + g_near.centre[1]; out[2] = v[2];
    return 1;
}

/* [api+0x10]+0x28 from the job consumer (return game 0xAB07E5): r9 points at entry +0x58 of the consumer's copy of the
 * job (0x1D0 bytes at rsp+0x50; +0x08 camera, +0x38/+0x39 gui offset/scale set, +0x40 gui, +0x48 offset, +0x50 scale) */
static uint64_t NearRun(uint64_t a, uint64_t camera, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h)
{
    if (g_nearActive && (uintptr_t)__builtin_return_address(0) == g_world.game + 0xAB07E5u && d > 0x58)
    {
        const uint8_t* job = (const uint8_t*)(uintptr_t)(d - 0x58);
        NearGuiFn offset = (NearGuiFn)NearApi(0xD0, 0x258), scale = (NearGuiFn)NearApi(0xD0, 0x270);
        uint8_t hasOffset = job[0x38], hasScale = job[0x39];
        uint64_t gui = 0;
        float o0[2] = {0, 0}, s0[2] = {1, 1};
        memcpy(&gui, job + 0x40, 8);
        if (hasOffset) memcpy(o0, job + 0x48, 8);
        if (hasScale) memcpy(s0, job + 0x50, 8);
        if (!(hasOffset || hasScale)) gui = g_near.gui;
        if (offset != NULL && scale != NULL && gui != 0)
        {
            if (camera == g_near.cam[0] || camera == g_near.cam[1])
            {
                offset(gui, g_near.offset[0] * s0[0] + o0[0], g_near.offset[1] * s0[1] + o0[1]);
                scale(gui, g_near.scale * s0[0], g_near.scale * s0[1]);
            }
            else if (!(hasOffset || hasScale)) { offset(gui, 0.0f, 0.0f); scale(gui, 1.0f, 1.0f); }
        }
    }
    return g_nearRunOriginal(a, camera, c, d, e, f, g, h);
}

static int NearRunInstall(void)
{
    if (g_nearRunOriginal != NULL) return 1;
    uintptr_t api = 0, table = 0;
    if (SafeRead(g_world.game + 0x3326308u, &api, 8) || api == 0 || SafeRead(api + 0x10, &table, 8) || table == 0) return 0;
    if (!ApiCodeMatches(g_world.game + 0xAB07E1u, "41 FF 52 28 FF C7 3B 7B 10") ||
        !ApiCodeMatches(g_world.game + 0xAB0721u, "41 FF 51 20 48 8B 05")) { Note("difficulty: near terrain: the job consumer is not the reviewed one"); return 0; }
    void** slot = (void**)(uintptr_t)(table + 0x28);
    DWORD old = 0;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) return 0;
    g_nearRunOriginal = (NearRunFn)*slot;
    InterlockedExchangePointer(slot, (void*)NearRun);
    VirtualProtect(slot, 8, old, &old);
    return 1;
}

/* The zone height materials' coordinates (RenderZoneHeightmap 0xF4A280 loads 1.0 at 0xF4A39C): 8.0 repeats them every
 * 2048 m as on a vanilla map (the tile's 0x3D only; see WorldDispatch's research knob). 1 when set. */
static int NearZoneRepeat(int on)
{
    uintptr_t load = g_world.game + 0xF4A39Cu + 5u;
    int32_t rel = 0;
    if (SafeRead(load, &rel, 4)) return 0;
    uintptr_t one = g_world.game + 0x23C6D70u, eight = g_world.game + 0x23C7524u, now = load + 4 + (intptr_t)rel;
    if (now != (on ? one : eight)) return 0;
    int32_t to = (int32_t)((intptr_t)(on ? eight : one) - (intptr_t)(load + 4));
    DWORD old = 0;
    if (!VirtualProtect((void*)load, 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *(volatile int32_t*)load = to;
    VirtualProtect((void*)load, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)load, 4);
    return 1;
}

/* The level's coarse caps in the near tile. RenderHeightmapVoronoi (0x3C, 0xF499B0) phase 1 draws each subregion's
 * route-cell area twice: its point heights (material 0x13DD2DCAD2968560, layer terrain_height, additive) and, when
 * the subregion type's descriptor has +0xD8 set (test r13, r13 / je at 0xF4A07B), again at the constant height
 * desc+0xE0 - desc+0xEC (material 0x369E29988294A16F, layer terrain_cap_height, MAX blend): a plateau with a hard edge.
 * Only 0x40's blur, fixed in texels, softens it. The tile replays the level's own 0x3C record (its ~300 m route-cell
 * polygons), so at the tile's 0.5 m texels each cap became a 19 m step over 16-20 m along straight edges hundreds of
 * metres long (run v18: straight bright lines in elevated views; the level's version is 110-280 m wide). For the
 * replayed level record only the je becomes a jmp: the tile's own cells still draw their caps, and the conform
 * (lowpass of coarse - tile) brings the level's plateau back as the level has it. 1 when patched. */
static int NearCapSkip(int on)
{
    uintptr_t at = g_world.game + 0xF4A07Bu;
    if (!ApiCodeMatches(at, on ? "4D 85 ED 74 6B" : "4D 85 ED EB 6B")) return 0;
    DWORD old = 0;
    if (!VirtualProtect((void*)(at + 3), 1, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *(volatile uint8_t*)(at + 3) = on ? 0xEB : 0x74;
    VirtualProtect((void*)(at + 3), 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)(at + 3), 1);
    return 1;
}

static uint32_t NearTexelBytes(uint32_t format)
{
    uint32_t bits = 0;
    if (format >= 45 || SafeRead((uintptr_t)GetModuleHandleW(NULL) + 0x1658990u + 4u * format, &bits, 4)) return 0;
    return bits / 8;
}

/* The level's own 0x3E is about to run: keep the intermediate captures it reads (lg+0..+0xBF). */
static void NearKeepCaptures(uint64_t context)
{
    if (g_near.capLevel == g_world.levels) return;
    g_near.capLevel = g_world.levels;
    uintptr_t gen = 0, lg = 0;
    if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0) { g_near.capLevel = -1; return; }
    lg = gen + 0x8758A8u; /* the merge's state (the 0x3C / 0x5E / 0x69 records' lg) */
    if (SafeRead(lg, g_near.caps, sizeof g_near.caps)) { g_near.capLevel = -1; return; }
    size_t total = 0;
    for (int k = 0; k < 8; k++)
    {
        uint32_t w, h, format; uint64_t data;
        memcpy(&w, g_near.caps + k * 0x18, 4); memcpy(&h, g_near.caps + k * 0x18 + 4, 4); memcpy(&format, g_near.caps + k * 0x18 + 8, 4);
        memcpy(&data, g_near.caps + k * 0x18 + 0x10, 8);
        uint32_t bytes = NearTexelBytes(format);
        if (g_near.capData[k] != NULL) { VirtualFree(g_near.capData[k], 0, MEM_RELEASE); g_near.capData[k] = NULL; }
        g_near.capBytes[k] = 0;
        if (data == 0 || w == 0 || h == 0 || w > 4096 || h > 4096 || bytes == 0) continue;
        size_t size = (size_t)w * h * bytes;
        uint8_t* copy = (uint8_t*)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (copy == NULL || SafeRead(data, copy, size)) { if (copy) VirtualFree(copy, 0, MEM_RELEASE); g_near.capLevel = -1; return; }
        g_near.capData[k] = copy; g_near.capBytes[k] = size; total += size;
        uint64_t mine = (uint64_t)(uintptr_t)copy;
        memcpy(g_near.caps + k * 0x18 + 0x10, &mine, 8);
    }
    Note("difficulty: near terrain: the level's intermediate captures kept for the tile (%.1f MB)", (double)total / 1048576.0);
}

static int NearOn(void)
{
    static int on = -1;
    if (on < 0) { char text[8] = ""; on = !(GetEnvironmentVariableA("HELLFORGE_XL_NEAR", text, sizeof text) > 0 && text[0] == '0'); }
    return on;
}

/* The level's heightmap at a world point (metres; row 0 at +y). */
static float NearGlobalAt(float wx, float wy)
{
    if (g_near.global == NULL) return -9999.0f;
    float gstep = 16384.0f / (float)g_near.columns;
    int c = (int)((wx + 8192.0f) / gstep), r = (int)g_near.rows - 1 - (int)((wy + 8192.0f) / gstep);
    if (c < 0 || r < 0 || c >= (int)g_near.columns || r >= (int)g_near.rows) return -9999.0f;
    return g_near.global[(size_t)r * g_near.columns + c];
}

/* The tile's heightmap at a world point (metres; its readback, normalized: v x 1000 - 500; row 0 at +y). */
static float NearTileAt(float wx, float wy)
{
    if (g_near.heights == NULL || g_near.hw == 0) return -9999.0f;
    float step = NEAR_SIDE / (float)g_near.hw;
    int c = (int)((wx - (g_near.centre[0] - NEAR_SIDE / 2)) / step), r = (int)(((g_near.centre[1] + NEAR_SIDE / 2) - wy) / step);
    if (c < 0 || r < 0 || c >= (int)g_near.hw || r >= (int)g_near.hh) return -9999.0f;
    return g_near.heights[(size_t)r * g_near.hw + c] * 1000.0f - 500.0f;
}

/* The first player's position against both heightmaps, every 1.5 s for a minute once the tile is in. */
static void NearReportPlayer(const float* at)
{
    WorldMemoryTick();
    static ULONGLONG last; static int reports; static LONG level = -1;
    if (g_near.global == NULL || g_near.level != g_world.levels || g_world.current != 8.0f) return;
    if (level != g_near.level) { level = g_near.level; reports = 0; last = 0; }
    ULONGLONG now = GetTickCount64();
    if (reports >= 40 || now - last < 1500) return;
    last = now; reports++;
    Note("difficulty: near terrain: player at (%.1f, %.1f, %.2f); the level's heightmap there %.2f, the tile's %.2f", (double)at[0], (double)at[1],
        (double)at[2], (double)NearGlobalAt(at[0], at[1]), (double)NearTileAt(at[0], at[1]));
}

/* ---- The drop point (the harness reads xl-drop.txt) --------------------------------------------------------------------
 * Water: GenerateWaterStart (op 0x0A, 0xF6C610) walks [[pass+0x58]] ids (u32, [pass+0x18]) into the area table
 * ([pass+0x10], pointers) and spawns a water body for each area whose level [area+0xE4] is above -500. The drop goes on
 * dry, gentle ground of the level's heightmap (the capture's float32 readback, row 0 at +y): 3 m above the highest water,
 * slope under 12 deg over +-16 m, under 20 m of relief within +-64 m, inside 1200 m of the centre. */
static struct { LONG level; int count; float high; } g_nearWater = {.level = -1};
static LONG g_nearDropLevel = -1;
static void TerrainNearDrop(void);

static void TerrainNearWater(uint64_t record)
{
    if (g_nearWater.level == g_world.levels) return;
    uint64_t pass = record + 8, ids = 0, table = 0, countAt = 0;
    uint32_t count = 0;
    if (SafeRead(pass + 0x58, &countAt, 8) || countAt == 0 || SafeRead(countAt, &count, 4) || SafeRead(pass + 0x18, &ids, 8) ||
        SafeRead(pass + 0x10, &table, 8) || ids == 0 || table == 0 || count > 65536) return;
    g_nearWater.level = g_world.levels; g_nearWater.count = 0; g_nearWater.high = -500.0f;
    for (uint32_t k = 0; k < count; k++)
    {
        uint32_t id = 0; uint64_t area = 0; float h = -1000.0f;
        if (SafeRead(ids + 4ull * k, &id, 4) || SafeRead(table + 8ull * id, &area, 8) || area == 0 || SafeRead(area + 0xE4, &h, 4)) continue;
        if (h > -500.0f) { g_nearWater.count++; if (h > g_nearWater.high) g_nearWater.high = h; }
    }
    Note("difficulty: near terrain: %d water bodies (of %u areas), the highest at %.2f m", g_nearWater.count, count, (double)g_nearWater.high);
}

static float NearHeight(int r, int c) { return g_near.global[(size_t)r * g_near.columns + c]; }

static void TerrainNearDrop(void)
{
    if (g_nearDropLevel == g_world.levels || g_near.global == NULL || g_nearWater.level != g_world.levels) return;
    g_nearDropLevel = g_world.levels;
    const float step = 16384.0f / (float)g_near.columns; /* 4 m */
    float bestX = 0, bestY = 0, bestSlope = 1e9f;
    int candidates = 0;
    for (float wy = -1200.0f; wy <= 1200.0f; wy += 64.0f)
        for (float wx = -1200.0f; wx <= 1200.0f; wx += 64.0f)
        {
            if (wx * wx + wy * wy > 1200.0f * 1200.0f) continue;
            int c = (int)((wx + 8192.0f) / step), r = (int)g_near.rows - 1 - (int)((wy + 8192.0f) / step);
            if (r < 16 || c < 16 || r >= (int)g_near.rows - 16 || c >= (int)g_near.columns - 16) continue;
            float h = NearHeight(r, c);
            if (h < g_nearWater.high + 3.0f) continue;
            float lo = h, hi = h;
            for (int dr = -16; dr <= 16; dr += 4) for (int dc = -16; dc <= 16; dc += 4)
            { float v = NearHeight(r + dr, c + dc); if (v < lo) lo = v; if (v > hi) hi = v; }
            if (hi - lo > 20.0f || lo < g_nearWater.high + 1.0f) continue;
            float gx = (NearHeight(r, c + 4) - NearHeight(r, c - 4)) / 32.0f, gy = (NearHeight(r - 4, c) - NearHeight(r + 4, c)) / 32.0f;
            float slope = atanf(sqrtf(gx * gx + gy * gy)) * 57.29578f;
            if (slope > 12.0f) continue;
            candidates++;
            float score = slope + 0.002f * sqrtf(wx * wx + wy * wy);
            if (score < bestSlope) { bestSlope = score; bestX = wx; bestY = wy; }
        }
    if (candidates == 0) { Note("difficulty: near terrain: no dry, gentle drop point within 1200 m; no drop file"); return; }
    char line[64];
    int len = snprintf(line, sizeof line, "%.1f,%.1f\n", (double)bestX, (double)bestY);
    HANDLE file = OpenLog(L"xl-drop.txt");
    if (file != INVALID_HANDLE_VALUE) { DWORD wrote; WriteFile(file, line, (DWORD)len, &wrote, NULL); CloseHandle(file); }
    g_worldDrop[0] = bestX; g_worldDrop[1] = bestY; g_worldDropLevel = g_world.levels;
    g_near.centre[0] = 256.0f * floorf(bestX / 256.0f + 0.5f); g_near.centre[1] = 256.0f * floorf(bestY / 256.0f + 0.5f);
    g_near.ready = 1;
    Note("difficulty: near terrain: drop point (%.0f, %.0f), ground %.2f m (water up to %.2f m), %d dry gentle candidates", (double)bestX, (double)bestY,
        (double)NearGlobalAt(bestX, bestY), (double)g_nearWater.high, candidates);
}

/* The level's merge records the tile replays, in the level's order, and the vanilla-density tiles after its 0x3C.
 * Not 0x94 (the flattening lookup), 0x3E (flatten zones) or 0x3F (pads): replayed on a tile they draw level-space data
 * squeezed into the tile, the whole 16 km level at 1 / 7.1 about the tile's centre (static, subagent geom14; s75p step
 * dumps: 0x3E cut a 88 x 96 m pit to 1.0 m at centre + w x 0.1406 of flatten type 30's cells, in the drop tile and two
 * mid-mission tiles; s75w C36: flatten zone 316 as a ~200 m deep pit beside open6; s75u A18: city 2 as a field of
 * needles round open3's objective, every tile's raw maximum the same 137.6 m). The lookup's only publisher is 0x94
 * (0xF3D32B) and 0x3F's phase 1 takes it down (0xC1650F), so no later op reads it. Under the pads and the flatten zones
 * the conform takes the level's own surface at full weight (g_pads, NearTarget), and the level has both. */
static int NearPlan(uint64_t records, uint32_t at)
{
    static const uint8_t ops[] = {0x39, 0x3C, 0x40, 0x3D, 0x0D, 0x63, 0x42, 0x41, 0x64};
    uint32_t start = 0, op = 0;
    for (uint32_t i = at; i > 0; i--)
        if (!SafeRead(records + (uint64_t)(i - 1) * 0x19B0u, &op, 4) && op == 0x38) { start = i; break; }
    if (start == 0) return 0;
    uint32_t seen = 0;
    g_near.steps = 0;
    for (uint32_t i = start; i < at && g_near.steps < NEAR_STEPS; i++)
    {
        if (SafeRead(records + (uint64_t)i * 0x19B0u, &op, 4)) return 0;
        size_t k = 0;
        while (k < sizeof ops && ops[k] != op) k++;
        if (k == sizeof ops || (seen & (1u << k))) continue;
        seen |= 1u << k;
        g_near.stepKind[g_near.steps] = NEAR_RECORD; g_near.stepRecord[g_near.steps] = i; g_near.stepOp[g_near.steps] = (uint8_t)op; g_near.steps++;
        if (op != 0x3C || g_terrain.level != g_world.levels || g_terrain.columns == 0) continue;
        /* the tiles whose drawn core meets the near square (generator coordinates: world + 8192) */
        const float core = (float)TERRAIN_TILE_CORE;
        float x0 = g_near.centre[0] + 8192.0f - NEAR_SIDE / 2, y0 = g_near.centre[1] + 8192.0f - NEAR_SIDE / 2;
        for (int t = 0; t < g_terrain.tiles && g_near.steps < NEAR_STEPS; t++)
        {
            float cx = (float)(t % g_terrain.columns) * core, cy = (float)(t / g_terrain.columns) * core;
            const float b = TERRAIN_BLEND; /* a tile draws out to its blend band */
            if (cx + core + b <= x0 || cx - b >= x0 + NEAR_SIDE || cy + core + b <= y0 || cy - b >= y0 + NEAR_SIDE || !TerrainTileWanted(t)) continue;
            g_near.stepKind[g_near.steps] = NEAR_TILE; g_near.stepTile[g_near.steps] = t; g_near.stepOp[g_near.steps] = 0x3C; g_near.steps++;
        }
    }
    return seen == (1u << sizeof ops) - 1;
}

static void NearCameras(float half0, float half1, float cx, float cy)
{
    NearExtentsFn extents = (NearExtentsFn)NearApi(0x20, 0x98);
    CamPoseFn position = (CamPoseFn)NearApi(0x20, 0x20);
    NearUnitFn update = (NearUnitFn)NearApi(0x08, 0x100);
    float at[3] = {cx, cy, 501.0f};
    for (int k = 0; k < 2; k++)
    {
        float h = k == 0 ? half0 : half1;
        extents(g_near.cam[k], -h, h, -h, h, 0);
        position(g_near.cam[k], g_near.node[k], at);
        update(g_near.world, g_near.node[k]);
    }
}

/* Puts the level's merge state back after the replay (or a failed one) and lets the level go on. */
static void NearReplayEnd(int ok)
{
    InterlockedExchange(&g_nearActive, 0);
    if (g_mat.on && g_mat.owner == MAT_TILE) InterlockedExchange(&g_mat.on, 0); /* the cameras go back below */
    g_near.matLeave = 0;
    if (g_near.R != 0)
    {
        NearCameras(g_near.world_size / 2, g_near.world_size / 2, 0.0f, 0.0f);
        NearGuiFn offset = (NearGuiFn)NearApi(0xD0, 0x258), scale = (NearGuiFn)NearApi(0xD0, 0x270);
        if (offset && scale && g_near.gui) { offset(g_near.gui, 0.0f, 0.0f); scale(g_near.gui, 1.0f, 1.0f); }
    }
    NearNameFn unpublish = (NearNameFn)NearApi(0x10, 0x218);
    for (int k = 0; k < 2; k++) if (g_near.tex[k] != 0 && unpublish != NULL) unpublish(kNearNames[k]);
    if (g_near.tex[0] != 0 && unpublish != NULL) unpublish(kNearRoadAlias);
    NearOneMipPublish(); /* the unpublish removed the names: the level's own copy goes back under them */
    if (!ok)
    {
        NearReleaseFn release = (NearReleaseFn)NearApi(0x10, 0x198);
        for (int k = 0; k < 2; k++) if (g_near.tex[k] != 0 && release != NULL) { release(g_near.tex[k]); g_near.tex[k] = 0; }
        g_near.state = NEAR_FAILED;
    }
}

/* ---- Op 0x41's material painters in vanilla space --------------------------------------------------------------------
 * RenderVoronoiMaterials (op 0x41; setup 0xF4F9E0, per frame 0xF52CC0) spawns the zone painters (units in the merge's
 * world, all with the one pose its setup builds at rbp+0xD0..+0x10F: rows loaded from game 0x263CC50 / 0x263CC60 /
 * 0x263CC70 / 0x263CC80 at 0xF4FACF / 0xF4FADB / 0xF4FAFC / 0xF4FB26, an identity) and draws them with the merge
 * cameras. The painters' vertex shaders (materials 11b26119177842e2, 5a503dfb17d3b948, 8448abc93ddacf85,
 * b01bc7148e58524c) have vanilla's world size compiled in: uv = (position x cb_world).xy x (1/2048, -1/2048) + 0.5, at
 * which the pixel shaders sample generated_heightmap_properties(_height), generated_route_distance and
 * generated_water_replace_lookup, and their noise at uv x 2048 / noise_scale (metres). On a 16 km level the uv runs
 * from -3.5 to 4.5: the whole world squeezed into the central 2 km and edge texels everywhere else (run v14: the
 * level's material map a central disc with "+" streaks out along rows and columns). The other two painter materials
 * (the zone mesh 1e02661b6eab49b7 at 0xF51AEE, the fallback quad b6e50e2b95081af1 at 0xF4FE97 / 0xF501E6) read only
 * cb_world and the camera.
 * So 0x41 draws in vanilla space: for a span L centred on c, s = 2048 / L, the pose rows (s,0,0,0), (0,s,0,0),
 * (0,0,1,0), (-s cx, -s cy, 0, 1) (the three loads' displacements point at the mod's rows for each 0x41 call, then
 * back), both merge cameras +-1024 at the origin while 0x41's jobs run (its tile job puts camera R+0x10 at
 * ((col + 0.5) W/T - W/2, ...), 0 for T = 1; its final job camera R+0 at 0), and noise_scale (0x53D111F6, set per
 * painter by 0xF48C50 through [[api+0x28]+0] at 0xF48D6B) x s, so the noise stays in metres. The textures the
 * painters sample cover the span drawn: the level's (whole world, its own 0x3C / 0x0D / 0x42) or the tile's (the
 * replay's, on the tile's cameras). The cameras change only once the job queue is empty and go back once 0x41's jobs
 * are drained (the queue holds a camera pointer; the consumer reads the camera when it runs the job). Any map larger
 * than vanilla's; off with HELLFORGE_XL_MATERIAL_SPACE=0. */
static const uint32_t kMatLoads[3] = {0xF4FACFu, 0xF4FADBu, 0xF4FB26u}; /* movaps xmmN, [rip+disp]: the displacement at +3 */
static const uint32_t kMatRows[3] = {0x263CC50u, 0x263CC60u, 0x263CC80u};
/* noise_scale (murmur64("noise_scale") >> 32), set per painter from the zone data by 0xF48C50: x s keeps the noise in
 * metres. The slope painter (5a503dfb17d3b948) also takes its slope from samples one 1/2048 uv step either side
 * (normal = normalize(dh_x, dh_y, 2): vanilla's 1 m texels), so in vanilla space at s < 1 it reads tan(theta) / s;
 * its band (angle_end, angle_gradient_scale) and terrain_properties_mip are not ones the zone data sets (run v18: the
 * data set angle_end = 0.21 for some painter, units not established; the other two never came through), so they
 * stay as the materials have them. */
#define MAT_NOISE_SCALE 0x53D111F6u
typedef uint64_t (*MatVarFn)(uint64_t object, uint64_t name, float value, uint64_t d);
static MatVarFn g_matVarOriginal;

static uint64_t MatVar(uint64_t object, uint64_t name, float value, uint64_t d)
{
    if (g_mat.patched && (uint32_t)name == MAT_NOISE_SCALE && (uintptr_t)__builtin_return_address(0) == g_world.game + 0xF48D6Eu &&
        GetCurrentThreadId() == g_mat.thread)
        value *= g_mat.s;
    return g_matVarOriginal(object, name, value, d);
}

static int MatOn(void)
{
    static int on = -1;
    if (on < 0) { char t[4] = ""; on = !(GetEnvironmentVariableA("HELLFORGE_XL_MATERIAL_SPACE", t, sizeof t) > 0 && t[0] == '0'); }
    return on;
}

/* Once: the reviewed code, the rows' block and the noise wrapper. 1 when ready. */
static int MatInstall(void)
{
    static int state; /* 0 not tried, 1 ready, -1 not possible */
    if (state != 0) return state > 0;
    state = -1;
    uintptr_t game = g_world.game;
    if (!ApiCodeMatches(game + 0xF4FACFu, "0F 28 05 7A D1 6E 01") || !ApiCodeMatches(game + 0xF4FADBu, "0F 28 0D 7E D1 6E 01") ||
        !ApiCodeMatches(game + 0xF4FAFCu, "0F 28 05 6D D1 6E 01") || !ApiCodeMatches(game + 0xF4FB26u, "0F 28 0D 53 D1 6E 01") ||
        !ApiCodeMatches(game + 0xF48D5Bu, "48 8B 05 A6 D5 3D 02 8B 11 48 8B CF 4C 8B 40 28 41 FF 10"))
    { Note("difficulty: extra large maps: the material pass is not the reviewed one; its painters draw as the game has them"); return 0; }
    g_mat.rows = AllocateNearGame(game, 0x1000);
    uintptr_t api = 0, table = 0;
    if (g_mat.rows == NULL || ((uintptr_t)g_mat.rows & 15) != 0 || SafeRead(game + 0x3326308u, &api, 8) || api == 0 || SafeRead(api + 0x28, &table, 8) ||
        table == 0)
    { Note("difficulty: extra large maps: no room for the material pass's pose; its painters draw as the game has them"); return 0; }
    for (int k = 0; k < 3; k++)
    {
        intptr_t reach = (intptr_t)(g_mat.rows + 16 * k) - (intptr_t)(game + kMatLoads[k] + 7);
        if (reach != (intptr_t)(int32_t)reach) { Note("difficulty: extra large maps: the material pose is out of reach"); return 0; }
    }
    void** slot = (void**)(uintptr_t)table;
    DWORD old = 0;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) return 0;
    g_matVarOriginal = (MatVarFn)*slot;
    InterlockedExchangePointer(slot, (void*)MatVar);
    VirtualProtect(slot, 8, old, &old);
    state = 1;
    return 1;
}

/* The three pose loads at the mod's rows (on) or the game's (off). */
static void TerrainMatPatch(int on)
{
    uintptr_t game = g_world.game, first = game + kMatLoads[0], last = game + kMatLoads[2] + 7;
    DWORD old = 0;
    if (on) { g_mat.thread = GetCurrentThreadId(); MemoryBarrier(); }
    if (!VirtualProtect((void*)first, last - first, PAGE_EXECUTE_READWRITE, &old)) return;
    for (int k = 0; k < 3; k++)
    {
        uintptr_t at = game + kMatLoads[k] + 3, next = game + kMatLoads[k] + 7;
        uintptr_t to = on ? (uintptr_t)(g_mat.rows + 16 * k) : game + kMatRows[k];
        *(volatile int32_t*)at = (int32_t)((intptr_t)to - (intptr_t)next);
    }
    VirtualProtect((void*)first, last - first, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)first, last - first);
    InterlockedExchange(&g_mat.patched, on);
}

static void MatCameras(float half0, float half1, float cx, float cy)
{
    NearExtentsFn extents = (NearExtentsFn)NearApi(0x20, 0x98);
    CamPoseFn position = (CamPoseFn)NearApi(0x20, 0x20);
    NearUnitFn update = (NearUnitFn)NearApi(0x08, 0x100);
    if (extents == NULL || position == NULL || update == NULL) return;
    float at[3] = {cx, cy, 501.0f};
    for (int k = 0; k < 2; k++)
    {
        float h = k == 0 ? half0 : half1;
        extents(g_mat.cam[k], -h, h, -h, h, 0);
        position(g_mat.cam[k], g_mat.node[k], at);
        update(g_mat.world, g_mat.node[k]);
    }
}

static uint32_t NearQueued(uint64_t context);

/* Vanilla space for a span L centred on (cx, cy), the cameras put back to back0 / back1 at (bx, by) after: 1 entered,
 * 0 wait (the queue still holds earlier jobs), -1 not possible (0x41 draws as the game has it). */
static int MatEnter(int owner, uint64_t context, uint64_t R, float span, float cx, float cy, float back0, float back1, float bx, float by)
{
    if (!MatOn() || !MatInstall()) return -1;
    uint32_t queued = NearQueued(context);
    if (queued != 0)
    {
        if (queued != 0xFFFFFFFFu && ++g_mat.waits < 1200) return 0;
        Note("difficulty: extra large maps: the job queue did not empty before the material pass; it draws as the game has it");
        g_mat.waits = 0;
        return -1;
    }
    if (SafeRead(R, &g_mat.cam[0], 8) || SafeRead(R + 8, &g_mat.node[0], 4) || SafeRead(R + 0x10, &g_mat.cam[1], 8) || SafeRead(R + 0x18, &g_mat.node[1], 4) ||
        SafeRead(context + 0x40, &g_mat.world, 8) || g_mat.cam[0] == 0 || g_mat.cam[1] == 0 || g_mat.world == 0 || !NearApi(0x20, 0x98) ||
        !NearApi(0x20, 0x20) || !NearApi(0x08, 0x100))
    { g_mat.waits = 0; return -1; }
    g_mat.s = 2048.0f / span;
    float rows[12] = {g_mat.s, 0, 0, 0, 0, g_mat.s, 0, 0, -g_mat.s * cx, -g_mat.s * cy, 0, 1};
    memcpy(g_mat.rows, rows, sizeof rows);
    g_mat.back[0] = back0; g_mat.back[1] = back1; g_mat.backAt[0] = bx; g_mat.backAt[1] = by;
    g_mat.owner = owner; g_mat.level = g_world.levels; g_mat.waits = 0; g_mat.quiet = 0;
    MatCameras(1024.0f, 1024.0f, 0.0f, 0.0f);
    MemoryBarrier();
    InterlockedExchange(&g_mat.on, 1);
    return 1;
}

/* After 0x41: once its jobs are drained, the cameras go back. 1 done, 0 wait. */
static int MatLeave(uint64_t context)
{
    if (!g_mat.on) return 1;
    uint32_t queued = NearQueued(context);
    if (queued != 0 && queued != 0xFFFFFFFFu && ++g_mat.waits < 1200) return 0;
    if (queued == 0 && ++g_mat.quiet < 4) return 0; /* the consumer's last job: a few frames */
    if (queued != 0) Note("difficulty: extra large maps: the material pass's jobs did not drain; the cameras go back now");
    MatCameras(g_mat.back[0], g_mat.back[1], g_mat.backAt[0], g_mat.backAt[1]);
    InterlockedExchange(&g_mat.on, 0);
    g_mat.waits = 0; g_mat.quiet = 0;
    return 1;
}

/* The level's dispatcher, before a record runs: 1 run it, 2 run it with the pose rows in place, 0 wait. */
static int TerrainMatDispatch(uint32_t op, uint64_t context)
{
    if (g_mat.on && g_mat.level != g_world.levels) InterlockedExchange(&g_mat.on, 0); /* a level that ended inside 0x41: its cameras are gone */
    if (g_mat.on && g_mat.owner == MAT_LEVEL && op != 0x41) return MatLeave(context);
    if (op != 0x41) return 1;
    uintptr_t gen = 0, settings = 0, R = 0;
    uint32_t size = 0, tiles = 1;
    if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0 || SafeRead(gen, &R, 8) || R == 0 || SafeRead(context + 8, &settings, 8) || settings == 0 ||
        SafeRead(settings + 0x2C, &size, 4) || size <= 2048u || size > 65536u)
        return 1;
    if (!g_mat.on)
    {
        SafeRead(gen + 0x8758A8u + 0x124u, &tiles, 4); /* lg+0x124: the merge's T (the second camera is +-size / 2T) */
        if (tiles == 0 || tiles > 16) tiles = 1;
        float half = (float)size / 2.0f;
        int e = MatEnter(MAT_LEVEL, context, R, (float)size, 0.0f, 0.0f, half, half / (float)tiles, 0.0f, 0.0f);
        if (e == 0) return 0;
        if (e < 0) return 1;
        static LONG noted = -1;
        if (noted != g_world.levels)
        { noted = g_world.levels; Note("difficulty: extra large maps: the material pass draws the %u m world in vanilla space (scale %.4f)", size, (double)g_mat.s); }
    }
    return g_mat.owner == MAT_LEVEL ? 2 : 1;
}

/* One replayed record (or tile) step: 1 when it finished, 0 while it runs, -1 when it failed. */
static int NearBaseOn(void);
static int NearBaseStep(uint64_t context);

static int NearRunStep(uint64_t records, uint32_t end, uint64_t context, uint64_t g, uint64_t h)
{
    int s = g_near.step;
    uintptr_t gen = 0;
    if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0) return -1;
    volatile uint32_t index = 0;
    volatile int calls = 0;
    LONG fault = 0;
    /* a mid-mission build: each step's start (which step is live when something else breaks) */
    if (g_moveBuilding && g_near.calls == 0 && !g_near.building && !g_near.matLeave && !g_near.stampLeave && g_near.loggedStep != s + 1)
    {
        g_near.loggedStep = s + 1;
        if (g_near.stepKind[s] == NEAR_TILE) Note("difficulty: moving tiles: step %d: composite tile %d", s, g_near.stepTile[s]);
        else Note("difficulty: moving tiles: step %d: op 0x%02X (record %u)", s, g_near.stepOp[s], g_near.stepRecord[s]);
    }
    if (g_near.stepKind[s] == NEAR_TILE)
    {
        if (!g_near.building)
        {
            if (!(TerrainBuild(g_near.stepTile[s], records, end, context, g, h, gen, 1) && TerrainDrawPrepare(records, gen)))
            {
                g_near.tilesFailed++; g_near.calls = 0;
                return 1; /* the level's own cells stay there */
            }
            g_near.building = 1; g_near.calls = 0;
        }
        int clip = TerrainClipBegin(g_near.stepTile[s], gen);
        fault = WorldTileGuarded(g_terrain.record, context, &index, &g_terrain.phase, g, h, 1, &calls);
        if (clip) TerrainClipEnd();
        if (fault) return -1; /* the merge's state is unknown after a fault */
        if (index == 0 && ++g_near.calls < TERRAIN_DRAW_CALLS) return 0;
        g_near.building = 0; g_near.calls = 0;
        if (index != 0) { WorldArenaReset(context); g_near.tilesDrawn++; } else g_near.tilesFailed++;
        return 1;
    }
    if (g_near.matLeave)
    {
        if (!MatLeave(context)) return 0;
        g_near.matLeave = 0;
        return 1;
    }
    if (g_near.stampLeave)
    {
        /* 0x63's job renders through the merge camera when the queue runs it: the camera stays at the offset until then */
        uint32_t q = NearQueued(context);
        if (q != 0 && q != 0xFFFFFFFFu && ++g_near.stampWaits < 1200) return 0;
        NearCameras(NEAR_SIDE / 2, NEAR_SIDE / 2, g_near.centre[0], g_near.centre[1]);
        g_near.stampLeave = 0;
        return q == 0 ? 1 : -1;
    }
    if (g_near.stepKind[s] == NEAR_RECORD && g_near.stepOp[s] == 0x3C && NearBaseOn()) return NearBaseStep(context);
    int mat = 0;
    if (g_near.stepOp[s] == 0x41)
    {
        /* the tile's materials in vanilla space: its span on the full target, the cameras back on the tile after */
        if (g_near.calls == 0 && !g_mat.on)
        {
            int e = MatEnter(MAT_TILE, context, g_near.R, NEAR_SIDE, g_near.centre[0], g_near.centre[1], NEAR_SIDE / 2, NEAR_SIDE / 2, g_near.centre[0],
                g_near.centre[1]);
            if (e == 0) return 0;
        }
        mat = g_mat.on && g_mat.owner == MAT_TILE;
    }
    if (g_near.calls == 0)
    {
        if (SafeRead(records + (uint64_t)g_near.stepRecord[s] * 0x19B0u, g_near.one, sizeof g_near.one)) return -1;
        switch (g_near.stepOp[s])
        {
        case 0x3C: memset(g_near.one + 8 + 0x108, 0, 8); break;
        case 0x94: memset(g_near.one + 0xB0, 0, 4); break;
        case 0x3E: memset(g_near.one + 0x80, 0, 0x40); break;
        case 0x3F: memset(g_near.one + 0x128, 0, 0x40); break;
        }
        g_near.phase = 0;
        /* a tile built in mission: 0x63 renders the world through the merge camera, and mid-mission the world holds the
         * live terrains, which it draws as heights too (run s72k: the step's delta was the level's surface, corr 0.978);
         * the tile's stamp terrains stand at the offset, where no terrain is, and the cameras look there for this step */
        if (g_near.stepOp[s] == 0x63 && (g_nearStampOffset[0] != 0 || g_nearStampOffset[1] != 0))
            NearCameras(NEAR_SIDE / 2, NEAR_SIDE / 2, g_near.centre[0] + g_nearStampOffset[0], g_near.centre[1] + g_nearStampOffset[1]);
    }
    uint8_t live[0xC0];
    /* 0x3E and 0x3F sample the intermediate captures lg+0..+0xBF on the CPU (0xF4AE30 via [api+0x10]+0x1F0; no other
     * replayed op does: the other callers are 0xB4 and 0x57). Both get the copies kept before the level's 0x3E, the
     * captures its own 0x3E and 0x3F read (0x5B/0x5C after 0x40; re-taken only after 0x0C). Mid-mission they are the
     * only ones: the level's 0x66 released the readbacks and zeroed their data (run s72e: 0x3F faulted at exe+0x31CCE5,
     * the sampler's float load). */
    int swapCaps = g_near.stepOp[s] == 0x3E || g_near.stepOp[s] == 0x3F;
    if (swapCaps) { memcpy(live, (void*)(uintptr_t)g_near.lg, sizeof live); memcpy((void*)(uintptr_t)g_near.lg, g_near.caps, sizeof live); }
    int zone = g_near.stepOp[s] == 0x3D && NearZoneRepeat(1);
    int caps = g_near.stepOp[s] == 0x3C && NearCapSkip(1);
    if (mat) TerrainMatPatch(1);
    fault = WorldTileGuarded(g_near.one, context, &index, &g_near.phase, g, h, 1, &calls);
    if (mat) TerrainMatPatch(0);
    if (caps) NearCapSkip(0);
    if (zone) NearZoneRepeat(0);
    if (swapCaps) memcpy((void*)(uintptr_t)g_near.lg, live, sizeof live);
    if (fault)
    {
        Note("difficulty: near terrain: replayed op 0x%02X faulted (%08lX at game.dll+0x%llX)", g_near.stepOp[s], (unsigned long)fault,
            (unsigned long long)(TileGuardSelf()->at - g_world.game));
        return -1;
    }
    g_near.calls++;
    if (index != 0)
    {
        g_near.calls = 0;
        WorldArenaReset(context);
        if (mat) { g_near.matLeave = 1; return 0; }
        if (g_near.stepOp[s] == 0x63 && (g_nearStampOffset[0] != 0 || g_nearStampOffset[1] != 0)) { g_near.stampLeave = 1; g_near.stampWaits = 0; return 0; }
        return 1;
    }
    return g_near.calls < 3000 ? 0 : -1;
}

static uint32_t NearQueued(uint64_t context)
{
    uint64_t queue = 0; uint32_t n = 0;
    if (SafeRead(context + 0x50, &queue, 8) || queue == 0 || SafeRead(queue + 0x10, &n, 4)) return 0xFFFFFFFFu;
    return n;
}

/* ---- Calibration (HELLFORGE_XL_NEAR_CALIBRATE=1, research): can the tile's heights be written from the CPU? -----------
 * RenderHeightmapVoronoi (0xF499B0) draws each cell as gui triangles (gui [R+0x110], [api+0xD0]+0x0 = exe 0x33D140:
 * gui, float3* p0, p1, p2, layer, float4* colour (255,0,0,0 from game 0x23C8540; quantized to bytes), material, uv*3
 * NULL) with the material [api+0xD0]+0x230(gui, 0x13DD2DCAD2968560), positions in pixels (metres x [lg+0x120] /
 * settings+0x2C, from the world corner, y up) and the height as the vertex z (heights x settings+0x30, a float all the
 * way on the CPU side), then queues one job on [ctx+0x50] (world, camera [R], generator [R+0x28], [R+0x108],
 * settings+0x74, id -1; ticket lock +0/+8 by 1). How z becomes the u16 texel, which target the job writes and whether a
 * draw replaces or blends are in shader data: after the tile's readback this draws four squares in the tile's
 * north-west corner (z = 0.52 / 0.56 x the scale; one drawn twice; one wound the other way), then (a) re-runs 0x64 alone
 * (before any square, to see that it reproduces the tile), (b) squares then 0x64, (c) squares then every step after
 * the Voronoi tiles, each with a readback, and notes what changed. The tile then spawns with the last result. */
static struct { int stage, from, to, phase; uint16_t* buf; uint32_t id, polls; } g_cal;

static int NearCalOn(void)
{
    static int on = -1;
    if (on < 0) { char t[4] = ""; on = GetEnvironmentVariableA("HELLFORGE_XL_NEAR_CALIBRATE", t, sizeof t) > 0 && t[0] == '1'; }
    return on;
}

/* The four squares: west-east along the north edge, 128 m each, 16 m in from the edges. */
static void NearCalSquare(int k, float* x0, float* y0, float* x1, float* y1)
{
    *x0 = g_near.centre[0] - NEAR_SIDE / 2 + 16.0f + 144.0f * (float)k; *x1 = *x0 + 128.0f;
    *y1 = g_near.centre[1] + NEAR_SIDE / 2 - 16.0f; *y0 = *y1 - 128.0f;
}

static int NearCalDraw(uint64_t context)
{
    typedef uint64_t (*MatFn)(uint64_t gui, uint64_t hash);
    typedef void (*TriFn)(uint64_t gui, const float* p0, const float* p1, const float* p2, uint32_t layer, const float* colour, uint64_t material,
        const float* uv0, const float* uv1, const float* uv2);
    MatFn mat = (MatFn)NearApi(0xD0, 0x230);
    TriFn tri = (TriFn)NearApi(0xD0, 0x0);
    float hs = 0, colour[4] = {0};
    uint64_t queue = 0, camera = 0, generator = 0, r108 = 0;
    if (mat == NULL || tri == NULL || SafeRead(g_near.settings + 0x30, &hs, 4) || SafeRead(g_world.game + 0x23C8540u, colour, 16) ||
        SafeRead(context + 0x50, &queue, 8) || queue == 0 || SafeRead(g_near.R, &camera, 8) || SafeRead(g_near.R + 0x28, &generator, 8) ||
        SafeRead(g_near.R + 0x108, &r108, 8)) return 0;
    uint64_t material = mat(g_near.gui, 0x13DD2DCAD2968560ull);
    const float px = g_near.samples / g_near.world_size, half = g_near.world_size / 2;
    const float z[4] = {0.52f * hs, 0.56f * hs, 0.52f * hs, 0.52f * hs};
    for (int k = 0; k < 4; k++)
    {
        float x0, y0, x1, y1;
        NearCalSquare(k, &x0, &y0, &x1, &y1);
        float a[3] = {(x0 + half) * px, (y0 + half) * px, z[k]}, b[3] = {(x1 + half) * px, (y0 + half) * px, z[k]};
        float c[3] = {(x1 + half) * px, (y1 + half) * px, z[k]}, d[3] = {(x0 + half) * px, (y1 + half) * px, z[k]};
        int times = k == 2 ? 2 : 1;
        for (int t = 0; t < times; t++)
        {
            if (k == 3) { tri(g_near.gui, a, c, b, 0, colour, material, NULL, NULL, NULL); tri(g_near.gui, a, d, c, 0, colour, material, NULL, NULL, NULL); }
            else { tri(g_near.gui, a, b, c, 0, colour, material, NULL, NULL, NULL); tri(g_near.gui, a, c, d, 0, colour, material, NULL, NULL, NULL); }
        }
    }
    /* the job, as 0xF4A195 queues it */
    volatile LONG64* ticket = (volatile LONG64*)(uintptr_t)queue;
    LONG64 mine = InterlockedExchangeAdd64(ticket, 1);
    while (*(volatile LONG64*)(uintptr_t)(queue + 8) != mine) YieldProcessor();
    uint32_t n = *(volatile uint32_t*)(uintptr_t)(queue + 0x10);
    uint8_t* e = (uint8_t*)(uintptr_t)(queue + 0x18 + (uint64_t)n * 0x1D0);
    *(volatile uint32_t*)(uintptr_t)(queue + 0x10) = n + 1;
    memcpy(e + 0x00, &g_near.world, 8); memcpy(e + 0x08, &camera, 8); memcpy(e + 0x10, &generator, 8); memcpy(e + 0x18, &r108, 8);
    memset(e + 0x20, 0, 4); e[0x24] = 0; memset(e + 0x38, 0, 2);
    SafeRead(g_near.settings + 0x74, e + 0x58, 16);
    { uint32_t id = 0xFFFFFFFFu; memcpy(e + 0x68, &id, 4); }
    InterlockedExchangeAdd64((volatile LONG64*)(uintptr_t)(queue + 8), 1);
    Note("difficulty: near terrain: calibration: 4 squares drawn (z %.1f / %.1f, height scale %.1f, material 0x%llX) and queued", (double)z[0], (double)z[1],
        (double)hs, (unsigned long long)material);
    return 1;
}

/* The readback in u16 (format 22 only): 1 when taken, 0 while waiting, -1 on failure. */
static int NearCalTake(void)
{
    NearReadyFn ready = (NearReadyFn)NearApi(0x10, 0x1D0);
    NearDescFn desc = (NearDescFn)NearApi(0x10, 0x1D8);
    if (ready == NULL || desc == NULL) return -1;
    if (!ready(g_cal.id)) return ++g_cal.polls < 2000 ? 0 : -1;
    uint8_t out[32] = {0}, d[24];
    const uint8_t* p = desc(out, g_cal.id);
    memcpy(d, p ? p : out, 24);
    uint32_t w, hgt, format; uint64_t data;
    memcpy(&w, d, 4); memcpy(&hgt, d + 4, 4); memcpy(&format, d + 8, 4); memcpy(&data, d + 16, 8);
    if (data == 0 || w != 4096 || hgt != 4096 || NearTexelBytes(format) != 2) return -1;
    if (g_cal.buf == NULL) g_cal.buf = (uint16_t*)VirtualAlloc(NULL, (size_t)w * hgt * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_cal.buf == NULL || SafeRead(data, g_cal.buf, (size_t)w * hgt * 2)) return -1;
    return 1;
}

/* What changed against the tile's first readback (g_near.heights, normalized): inside each square and outside them. */
static void NearCalReport(const char* what)
{
    double inSum[4] = {0}, inBefore[4] = {0}; int inN[4] = {0}, outChanged = 0; float outMax = 0;
    const float side = NEAR_SIDE / 4096.0f;
    for (uint32_t r = 0; r < 4096; r++)
        for (uint32_t c = 0; c < 4096; c++)
        {
            size_t i = (size_t)r * 4096 + c;
            float before = g_near.heights[i] * 1000.0f - 500.0f, after = g_cal.buf[i] / 65535.0f * 1000.0f - 500.0f;
            float wx = g_near.centre[0] - NEAR_SIDE / 2 + (c + 0.5f) * side, wy = g_near.centre[1] + NEAR_SIDE / 2 - (r + 0.5f) * side;
            int in = -1;
            for (int k = 0; k < 4 && in < 0; k++)
            {
                float x0, y0, x1, y1; NearCalSquare(k, &x0, &y0, &x1, &y1);
                if (wx > x0 + 2 && wx < x1 - 2 && wy > y0 + 2 && wy < y1 - 2) in = k;
            }
            if (in >= 0) { inSum[in] += after; inBefore[in] += before; inN[in]++; }
            else if (fabsf(after - before) > 0.02f) { outChanged++; if (fabsf(after - before) > outMax) outMax = fabsf(after - before); }
        }
    Note("difficulty: near terrain: calibration %s: squares (mean before -> after, m) %.2f -> %.2f, %.2f -> %.2f, %.2f -> %.2f (twice), %.2f -> %.2f (wound back); "
        "outside: %d texels changed by over 2 cm, max %.2f m", what,
        inN[0] ? inBefore[0] / inN[0] : 0.0, inN[0] ? inSum[0] / inN[0] : 0.0, inN[1] ? inBefore[1] / inN[1] : 0.0, inN[1] ? inSum[1] / inN[1] : 0.0,
        inN[2] ? inBefore[2] / inN[2] : 0.0, inN[2] ? inSum[2] / inN[2] : 0.0, inN[3] ? inBefore[3] / inN[3] : 0.0, inN[3] ? inSum[3] / inN[3] : 0.0,
        outChanged, (double)outMax);
    /* a profile across square 0's west edge (row through its middle): before / after every 2 m */
    float x0, y0, x1, y1; NearCalSquare(0, &x0, &y0, &x1, &y1);
    uint32_t r = (uint32_t)(((g_near.centre[1] + NEAR_SIDE / 2) - (y0 + y1) / 2) / side);
    char text[400]; int len = 0;
    for (int k = 0; k < 12; k++)
    {
        uint32_t c = (uint32_t)((x0 - 12.0f + 2.0f * k - (g_near.centre[0] - NEAR_SIDE / 2)) / side);
        len += snprintf(text + len, sizeof text - len, " %.1f/%.1f", (double)(g_near.heights[(size_t)r * 4096 + c] * 1000.0f - 500.0f),
            (double)(g_cal.buf[(size_t)r * 4096 + c] / 65535.0f * 1000.0f - 500.0f));
    }
    Note("difficulty: near terrain: calibration %s: across square 0's west edge (x0 - 12 m, every 2 m, before/after):%s", what, text);
}

/* The calibration's stages; 1 while running, 0 when done (the tile goes on with the last readback). */
static int NearCalStep(uint64_t records, uint32_t end, uint64_t context, uint64_t g, uint64_t h)
{
    static const char* const names[3] = {"(a) 0x64 again", "(b) squares + 0x64", "(c) squares + every step after the Voronoi tiles"};
    if (g_cal.phase == 0) /* set up the stage */
    {
        int last3C = -1, s64 = -1;
        for (int s = 0; s < g_near.steps; s++) { if (g_near.stepOp[s] == 0x3C) last3C = s; if (g_near.stepOp[s] == 0x64) s64 = s; }
        if (last3C < 0 || s64 < 0) return 0;
        if (g_cal.stage >= 1 && !NearCalDraw(context)) { Note("difficulty: near terrain: calibration: the squares were not drawn"); return 0; }
        g_cal.from = g_cal.stage == 2 ? last3C + 1 : s64; g_cal.to = s64;
        g_near.step = g_cal.from; g_near.calls = 0; g_near.building = 0;
        g_cal.phase = 1;
        return 1;
    }
    if (g_cal.phase == 1)
    {
        int r = NearRunStep(records, end, context, g, h);
        if (r < 0) { Note("difficulty: near terrain: calibration: step %d (op 0x%02X) failed", g_near.step, g_near.stepOp[g_near.step]); return 0; }
        if (r > 0 && g_near.step++ == g_cal.to) { g_cal.phase = 2; g_near.waits = 0; }
        return 1;
    }
    if (g_cal.phase == 2)
    {
        if (NearQueued(context) != 0 || ++g_near.waits < 4) return g_near.waits < 600 ? 1 : 0;
        NearReadbackFn readback = (NearReadbackFn)NearApi(0x10, 0x1A8);
        if (readback == NULL) return 0;
        g_cal.id = readback(g_near.tex[0]); g_cal.polls = 0; g_cal.phase = 3;
        return 1;
    }
    int t = NearCalTake();
    if (t == 0) return 1;
    if (t < 0) { Note("difficulty: near terrain: calibration: the readback failed"); return 0; }
    NearCalReport(names[g_cal.stage]);
    g_cal.phase = 0;
    if (++g_cal.stage < 3) return 1;
    /* the tile spawns with what the texture now holds */
    for (size_t i = 0; i < (size_t)4096 * 4096; i++) g_near.heights[i] = g_cal.buf[i] / 65535.0f;
    return 0;
}

/* ---- Conform: the tile takes the coarse terrain's shape and keeps its own detail (HELLFORGE_XL_NEAR_CONFORM=0: off) ----
 * The tile's replay is the vanilla-density generation of its square, but the coarse 16 km terrain around it is the
 * level's own (smoothed at 4 m a sample): they disagree by metres to tens of metres at the tile's edge (a seam) and
 * at large scale. A gui triangle drawn with the merge's Voronoi material ADDS its vertex z, in metres, to the heights
 * finalize (0x64) reads, and re-running 0x64 reproduces the tile exactly (calibration run v3: z 266.2 -> +266.24 m,
 * 286.7 -> +286.72, twice -> twice, either winding, sharp to the texel). So after the tile's first readback T:
 * 1. the level's final heightmap (the render config's generated_heightmap, which the level's 0x64 wrote) is read back:
 *    the coarse terrain C, 4096^2 over 16 km (sample i at -8191.75 + i x 16383.5 / 4095, row 0 at +y);
 * 2. the target is H = T + lowpass(C - T) inside (C bilinear at each tile texel; lowpass three box passes of
 *    2 NEAR_SHIFT_RADIUS + 1 texels, sigma ~64 m): the tile's own terrain, moved by the smooth part of its mismatch with
 *    the coarse, so a feature both have (a base's plateau, a hill) appears once, at the tile's detail; blended as
 *    H = C + w (T + lowpass(C - T) - C), w = 0 within 16 m of the edge rising smoothly to 1 at 160 m: exactly the
 *    coarse surface at the edge, where the tile's samples lie on the coarse terrain's (NEAR_HIDE). (Run v12, the first with the real coarse: C + (T - lowpass(T)) counted shared
 *    features twice, a base's walls ~20 m high, slope p99 31 deg.)
 * 3. H - T is drawn as a 2 m grid of triangles (vertices on texel centres, the outer ones half a texel beyond the edge
 *    so the edge texels are covered once), in batches of up to NEAR_BATCH triangles, one job each, each batch after
 *    the queue has emptied; then 0x64 again and a readback, which becomes the tile (and is compared with H).
 *    The cells within NEAR_FINE of the hidden square's edge are drawn a texel a cell (g_conform.ring: H - T at every
 *    texel there), so the tile is the coarse surface to the centimetre where the coarse terrain's free edge lies. */
#define NEAR_BATCH 16384
#define NEAR_SHIFT_RADIUS 128 /* the shift's box passes: 257 texels (~129 m), three of them */
#define NEAR_GRID 1025 /* grid lines: texel -0.5, 4, 8, ..., 4092, 4095.5 */
#define NEAR_BLOCKS ((NEAR_GRID - 1) / 2) /* 2 x 2 grid cells: a 4.5 m block */
static struct { int phase; uint32_t id, polls, next, waits; uint16_t* coarse; float* low; float* delta; float* ring; double ms; int seq[4], seqN, seqI;
    uint8_t* block; int allBlocks; uint32_t blocksCoarse; /* blocks drawn as two triangles (NearConformDraw) */
    volatile LONG planDone, planRunning; int planOk; ULONGLONG planSince; } g_conform;

/* Metres outside the hidden square (negative inside), max norm. */
static float NearOutside(float wx, float wy)
{
    return fmaxf(fabsf(wx - g_near.centre[0]), fabsf(wy - g_near.centre[1])) - NEAR_HIDE / 2;
}

/* The tile's band below the coarse terrain at e metres past the hidden square: 0 to NEAR_BAND_FLAT, then a gentle sink
 * (quadratic from a flat start) to NEAR_BAND_SINK over NEAR_BAND_EASE, then an ease-out to NEAR_BAND_DEPTH at
 * NEAR_BAND_END. The terrain pixel shader's normal is a central difference over +-1 texel (0.5625 m) of mip 0, so a tile
 * pixel still above the coarse surface must not have a texel of the dive within a texel of it: with the dive right at
 * the crossing (s70x: 16 m by 12 m from e = 4) the last half metre before the crossing had normals 30-56 deg off the
 * coarse terrain's, a dark line along every edge (offline: s70x dumps, three profiles per edge). The sink is under the
 * coarse surface by 0.03 m at 0.8 m past NEAR_BAND_FLAT, tilted 4 deg there; the texel before the dive is 0.26 m under. */
static float NearBand(float e)
{
    if (e <= NEAR_BAND_FLAT) return 0.0f;
    float u = (e - NEAR_BAND_FLAT) / NEAR_BAND_EASE;
    if (u <= 1) return NEAR_BAND_SINK * u * u;
    float v = (e - NEAR_BAND_FLAT - NEAR_BAND_EASE) / (NEAR_BAND_END - NEAR_BAND_FLAT - NEAR_BAND_EASE); v = v > 1 ? 1 : v;
    return NEAR_BAND_SINK + (NEAR_BAND_DEPTH - NEAR_BAND_SINK) * v * (2.0f - v);
}

/* Grid cells drawn a texel a cell: within NEAR_FINE m of the hidden square's edge, where the tile must be the coarse
 * surface to the centimetre (on the 2 m grid alone the tile kept its own detail between the grid's vertices: s70p,
 * up to 0.42 m). */
#define NEAR_FINE (g_xp.fine) /* past the band's sink (NEAR_BAND_FLAT + NEAR_BAND_EASE) by two texels and more */
static int NearFineCell(int i, int j);
static int NearChooseCentre(float out[2], int picker);

static int NearConformOn(void)
{
    static int on = -1;
    if (on < 0) { char t[4] = ""; on = !(GetEnvironmentVariableA("HELLFORGE_XL_NEAR_CONFORM", t, sizeof t) > 0 && t[0] == '0'); }
    return on;
}

/* One box pass of width 2r+1 along rows (step 1) or columns (step 4096), edge texels repeated. */
static void NearBox(const float* in, float* out, int r, int columns)
{
    for (int line = 0; line < 4096; line++)
    {
        const float* a = columns ? in + line : in + (size_t)line * 4096;
        float* b = columns ? out + line : out + (size_t)line * 4096;
        size_t step = columns ? 4096 : 1;
        double sum = 0;
        for (int k = -r; k <= r; k++) sum += a[(size_t)(k < 0 ? 0 : k > 4095 ? 4095 : k) * step];
        for (int i = 0; i < 4096; i++)
        {
            b[(size_t)i * step] = (float)(sum / (2 * r + 1));
            int add = i + r + 1, drop = i - r;
            sum += a[(size_t)(add > 4095 ? 4095 : add) * step] - a[(size_t)(drop < 0 ? 0 : drop) * step];
        }
    }
}

static float NearCoarseAt(float wx, float wy)
{
    const float step = 16383.5f / 4095.0f;
    float fx = (wx + 8191.75f) / step, fy = (8191.75f - wy) / step;
    if (fx < 0) fx = 0; if (fy < 0) fy = 0; if (fx > 4094.999f) fx = 4094.999f; if (fy > 4094.999f) fy = 4094.999f;
    uint32_t c = (uint32_t)fx, r = (uint32_t)fy;
    float ax = fx - c, ay = fy - r;
    const uint16_t* r0 = g_conform.coarse + (size_t)r * 4096, *r1 = r0 + 4096;
    float top = r0[c] * (1 - ax) + r0[c + 1] * ax, bottom = r1[c] * (1 - ax) + r1[c + 1] * ax;
    return (top * (1 - ay) + bottom * ay) / 65535.0f * 1000.0f - 500.0f;
}

/* The coarse terrain as its finest level draws it: each 4 m cell split by the index buffer (exe 0x2F7B20) into
 * {SW, NW, SE} and {SE, NW, NE} (vertex (i, j) is sample (c = i, r = N - 1 - j)); the band past the hidden square
 * follows these triangles, not the bilinear surface, which lies up to ~0.3 m off them on rough ground (offline checker
 * on s70z: near-camera tilt 26.6 -> 14.4 deg, the band face under displacement 0.034 -> 0 m). */
static float NearCoarseTri(float wx, float wy)
{
    const float step = 16383.5f / 4095.0f;
    float fx = (wx + 8191.75f) / step, fy = (8191.75f - wy) / step;
    if (fx < 0) fx = 0; if (fy < 0) fy = 0; if (fx > 4094.999f) fx = 4094.999f; if (fy > 4094.999f) fy = 4094.999f;
    uint32_t c = (uint32_t)fx, r = (uint32_t)fy;
    float ax = fx - c, ay = fy - r; /* ax east, ay south */
    const uint16_t* north = g_conform.coarse + (size_t)r * 4096, *south = north + 4096;
    float nw = north[c], ne = north[c + 1], sw = south[c], se = south[c + 1];
    float z = ax <= ay ? sw + ax * (se - sw) + (1 - ay) * (nw - sw) : ne + (1 - ax) * (nw - ne) + ay * (se - ne);
    return z / 65535.0f * 1000.0f - 500.0f;
}

static float NearGridTexel(int k) { return k == 0 ? -0.5f : k == NEAR_GRID - 1 ? 4095.5f : 4.0f * (float)k; }

static int NearFineCell(int i, int j)
{
    const float x0 = g_near.centre[0] - NEAR_SIDE / 2 + NEAR_TEXEL / 2, y0 = g_near.centre[1] + NEAR_SIDE / 2 - NEAR_TEXEL / 2;
    float u = (NearGridTexel(i) + NearGridTexel(i + 1)) / 2, v = (NearGridTexel(j) + NearGridTexel(j + 1)) / 2;
    return fabsf(NearOutside(x0 + NEAR_TEXEL * u, y0 - NEAR_TEXEL * v)) <= NEAR_FINE;
}

/* ---- The level's pads under the tile ------------------------------------------------------------------------------
 * Op 0x3F (RenderStampsHeightmap, game 0xF4CCA0; static: scratchpad pad3f.md) sets the heightmap to a pad height h
 * under every enabled circle and rectangle of each stamp's and location's shape (a flat core, then a smoothstep band;
 * the draw blends toward h by alpha, so a core with strength 1 is exactly h). A base's stamp units keep the pose they
 * got from the level's heights; its layer units sample the tile (NearLayerHeight). The capture sampler (0xF4AE30) reads
 * points in world units (its 1/2048 loads are world sites, world_sites.h), so the level and the tile's replay flatten
 * the same pads to the same h, but the conform's
 * target T + lowpass(C - T) would move a tile pad by the smooth mismatch around it (1-3 m). So under the pads the
 * target is the level's own surface C, weighted by W = strength x (1 - ss(t)) as the pad shaders weight it, with each
 * band at its largest (e = 1): circle r + T+0x3C ln(r + 1), rectangle a + T+0x3C ln(a + 1) and b + T+0x3C ln(b + 1).
 * Records (0xF4D129..0xF4D2C7): stamps [pass+0x10], 0x108 each, count [[pass+0x80]], P = [el+0x98], taken when
 * [[P+0x90]+0x160] & 0x400; locations [pass+0x18], 0xE0 each, count [[pass+0x90]], P = the record, skipped when
 * P+0x90 is null or the dword at P+0x98 is 0xC33EB5DC, or is 0xC33EB5DB without [P+0x90]+0x60 bit 0. P+0/+4 metres from
 * the world corner, P+8 the rotation; shape [[P+0x90]]: circles [shape] x [shape+8] (0x14: x, y, r, strength, flags
 * bit 0 = on), rectangles [shape+0x10] x [shape+0x18] (0x1C: x, y, a, b, angle, strength, flags). An offset is placed at
 * (ox sin + oy cos, oy sin - ox cos) + P (0xFC84A0); a rectangle's axes at the rotation - pi/2 + its angle. The
 * zone-type checks that skip a few stamps (0xF4D1B5, 0xF4D431) are not repeated: their areas take the level's surface. */
#define NEAR_PADS 16384 /* s75p had 5196 shapes, s75u 2580 */
typedef struct { float cx, cy, c, s, a, b, ba, bb, k; int rect, foot; } NearPad;
static NearPad g_padStore[NEAR_PADS]; /* zero-initialised (.bss) */
static struct { LONG level; int count, dropped; float band3C; NearPad* pad; float* w; LONG wLevel; float wCentre[2]; } g_pads = {.level = -1, .pad = g_padStore, .wLevel = -1};
#define NEAR_PAD_STEP 2.0f
#define NEAR_PAD_N 1152 /* NEAR_SIDE / NEAR_PAD_STEP */

/* strength: the element's strength x the item's [item+0x58] (0xF4D98B circles, 0xF4DC31 rectangles). Band: circles
 * B ln(a+1) metres (stored in pixels, 0xF4D963); rectangles the padMetre share of it (PadBandStubs: k / 2 on an extra
 * large level, the shader's metres). Op 0x3F also scales both bands by e <= 1 (0xF4D6DA: the ground's spread under the
 * footprint, from 0xF4AE30); 1 here, never narrower than the game's. */
static void NearPadAdd(float x, float y, float rot, const float* f, int rect, float size, float itemStrength)
{
    float k = (rect ? f[5] : f[3]) * itemStrength;
    if (!(k > 0.0f) || g_pads.count >= NEAR_PADS) { if (k > 0.0f) g_pads.dropped++; return; }
    float sn = sinf(rot), cs = cosf(rot);
    NearPad* p = &g_pads.pad[g_pads.count];
    p->cx = f[0] * sn + f[1] * cs + x - size / 2; p->cy = f[1] * sn - f[0] * cs + y - size / 2;
    p->k = k > 1.0f ? 1.0f : k; p->rect = rect; p->foot = 0;
    const float B = g_pads.band3C > 0.0f && g_pads.band3C < 1000.0f ? g_pads.band3C : 8.0f;
    if (rect)
    {
        float psi = rot - 1.5707963f + f[4];
        p->c = cosf(psi); p->s = sinf(psi); p->a = f[2]; p->b = f[3];
        const float m = g_world.values != NULL && g_world.values->padMetre > 0.0f ? g_world.values->padMetre : 1.0f;
        p->ba = fmaxf(m * B * logf(f[2] + 1.0f), 2.0f); p->bb = fmaxf(m * B * logf(f[3] + 1.0f), 2.0f);
    }
    else { p->a = f[2]; p->ba = fmaxf(B * logf(f[2] + 1.0f), 2.0f); }
    if (p->a >= 0.0f && p->a < 4096.0f && (!rect || (p->b >= 0.0f && p->b < 4096.0f))) g_pads.count++;
}

/* A stamp's whole footprint (radius [[P+0x90]+0xD8], the r_fp 0x3F reads for its stamps at 0xF4D206) at full weight,
 * with a band of half the radius (16 m at least): a stamp unit keeps the pose it got on the level's surface, and the
 * layer units of its content snap to the tile (NearLayerHeight), so under the footprint the tile must be the level's
 * surface or the pieces end up off each other (s70u: a crate in mid-air on a base at the drop point). Every stamp
 * element counts, not only those with pads (flag 0x400). 1 when added. */
static int NearPadFootprint(uint64_t P, float size)
{
    float pos[2], r = 0; uint64_t item = 0;
    if (SafeRead(P, pos, 8) || SafeRead(P + 0x90, &item, 8) || item == 0 || SafeRead(item + 0xD8, &r, 4) || !(r >= 1.0f && r <= 400.0f)) return 0;
    if (g_pads.count >= NEAR_PADS) { g_pads.dropped++; return 0; }
    NearPad* p = &g_pads.pad[g_pads.count++];
    p->cx = pos[0] - size / 2; p->cy = pos[1] - size / 2; p->k = 1.0f; p->rect = 0;
    p->a = r; p->ba = fmaxf(g_xp.footBand * r, g_xp.footBandMin); p->c = 1; p->s = 0; p->b = 0; p->bb = 0; p->foot = 1;
    return 1;
}

static void NearPadShape(uint64_t P, float size)
{
    float pos[3]; uint64_t item = 0, shape = 0, list = 0; uint32_t n = 0;
    float strength = 1.0f;
    if (SafeRead(P, pos, 12) || SafeRead(P + 0x90, &item, 8) || item == 0 || SafeRead(item, &shape, 8) || shape == 0) return;
    if (SafeRead(item + 0x58, &strength, 4) || !(strength >= 0.0f && strength <= 16.0f)) strength = 1.0f;
    for (int rect = 0; rect < 2; rect++)
    {
        if (SafeRead(shape + (rect ? 0x10 : 0), &list, 8) || SafeRead(shape + (rect ? 0x18 : 8), &n, 4) || list == 0 || n > 4096) continue;
        for (uint32_t i = 0; i < n; i++)
        {
            float f[7]; uint32_t flags = 0, stride = rect ? 0x1C : 0x14;
            if (SafeRead(list + (uint64_t)i * stride, f, rect ? 24 : 16) || SafeRead(list + (uint64_t)i * stride + (rect ? 0x18 : 0x10), &flags, 4)) break;
            if (flags & 1) NearPadAdd(pos[0], pos[1], pos[2], f, rect, size, strength);
        }
    }
}

/* The level's own 0x3F (the first of the level; the tile's replay runs it again later): its pads, as listed above. */
static void TerrainNearPads(uint64_t record, uint64_t context)
{
    if (g_pads.level == g_world.levels) return;
    g_pads.level = g_world.levels; g_pads.count = 0; g_pads.dropped = 0; g_pads.band3C = 0;
    uint64_t pass = record + 8, settings = 0, a = 0, b = 0, T = 0, base = 0, countAt = 0; uint32_t size = 0, count = 0;
    if (SafeRead(context + 8, &settings, 8) || settings == 0 || SafeRead(settings + 0x2C, &size, 4) || size < 2048u || size > 65536u) return;
    if (!SafeRead(context + 0x18, &a, 8) && a != 0 && !SafeRead(a + 0x2D0, &b, 8) && b != 0 && !SafeRead(b + 0x68, &T, 8) && T != 0) SafeRead(T + 0x3C, &g_pads.band3C, 4);
    uint32_t stamps = 0, locations = 0, footprints = 0;
    if (!SafeRead(pass + 0x10, &base, 8) && base != 0 && !SafeRead(pass + 0x80, &countAt, 8) && countAt != 0 && !SafeRead(countAt, &count, 4) && count < 65536)
        for (uint32_t i = 0; i < count; i++)
        {
            uint64_t P = 0, item = 0; uint32_t flags = 0;
            if (SafeRead(base + (uint64_t)i * 0x108 + 0x98, &P, 8) || P == 0 || SafeRead(P + 0x90, &item, 8) || item == 0) continue;
            footprints += (uint32_t)NearPadFootprint(P, (float)size);
            if (SafeRead(item + 0x160, &flags, 4) || !(flags & 0x400)) continue;
            NearPadShape(P, (float)size); stamps++;
        }
    if (!SafeRead(pass + 0x18, &base, 8) && base != 0 && !SafeRead(pass + 0x90, &countAt, 8) && countAt != 0 && !SafeRead(countAt, &count, 4) && count < 65536)
        for (uint32_t i = 0; i < count; i++)
        {
            uint64_t P = base + (uint64_t)i * 0xE0, item = 0; uint32_t kind = 0; uint8_t bit = 0;
            if (SafeRead(P + 0x90, &item, 8) || item == 0 || SafeRead(P + 0x98, &kind, 4) || kind == 0xC33EB5DCu) continue;
            if (kind == 0xC33EB5DBu && (SafeRead(item + 0x60, &bit, 1) || !(bit & 1))) continue;
            NearPadShape(P, (float)size); locations++;
        }
    /* The flatten zones (0x3E, not replayed on tiles): subregions G+0x92F328 (0x20: type index +0xC, route cell +0x18;
     * count G+0x11A71C4) whose type has a flatten margin +0x208 != FLT_MAX, the route cell's box +0x88..+0x94 (generator
     * metres) grown by it, as difficulty_world_macro.c holds them. Full weight over the grown box, a 16 m band past it. */
    uint32_t zones = 0, subs = 0, types = 0; uint64_t gen = 0, ts = 0, table = 0;
    if (g_worldLevel != 0 && !SafeRead(g_worldLevel + 0x2C8, &gen, 8) && gen != 0 && !SafeRead(g_worldLevel + 0x2D0, &ts, 8) && ts != 0 &&
        !SafeRead(ts, &table, 8) && table != 0 && !SafeRead(ts + 0xA0, &types, 4) && !SafeRead(gen + 0x11A71C4u, &subs, 4) && subs < 65536)
        for (uint32_t i = 0; i < subs; i++)
        {
            int32_t t = -1; uint64_t rc = 0, d = 0; float margin = 3.4e38f, box[4];
            if (SafeRead(gen + 0x92F328u + 0x20ull * i + 0xC, &t, 4) || t < 0 || (uint32_t)t >= types || SafeRead(gen + 0x92F328u + 0x20ull * i + 0x18, &rc, 8) ||
                rc == 0 || SafeRead(table + 8ull * (uint32_t)t, &d, 8) || d == 0 || SafeRead(d + 0x208, &margin, 4) || !(margin >= 0.0f && margin <= 2000.0f) ||
                SafeRead(rc + 0x88, box, 16)) continue;
            float x0 = fminf(box[0], box[2]) - margin, x1 = fmaxf(box[0], box[2]) + margin, y0 = fminf(box[1], box[3]) - margin, y1 = fmaxf(box[1], box[3]) + margin;
            if (!(x1 - x0 > 0.0f && y1 - y0 > 0.0f && x1 - x0 < 6000.0f && y1 - y0 < 6000.0f)) continue;
            if (g_pads.count >= NEAR_PADS) { g_pads.dropped++; continue; }
            NearPad* p = &g_pads.pad[g_pads.count++];
            p->cx = (x0 + x1) / 2 - (float)size / 2; p->cy = (y0 + y1) / 2 - (float)size / 2; p->k = 1.0f; p->rect = 1; p->foot = 0;
            p->c = 1.0f; p->s = 0.0f; p->a = (x1 - x0) / 2; p->b = (y1 - y0) / 2; p->ba = 16.0f; p->bb = 16.0f;
            zones++;
        }
    Note("difficulty: near terrain: the level's pads: %d shapes (%u stamp footprints, %u stamps with pads, %u locations, %u flatten zones; %d not kept), band scale %.2f",
        g_pads.count, footprints, stamps, locations, zones, g_pads.dropped, (double)g_pads.band3C);
    /* where they are (s71b: none reached the tile): their centres' extent and the 8 nearest the world's centre */
    if (g_pads.count > 0)
    {
        float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
        int order[8]; float best[8]; int kept = 0;
        for (int q = 0; q < g_pads.count; q++)
        {
            const NearPad* p = &g_pads.pad[q];
            if (p->cx < lo[0]) lo[0] = p->cx; if (p->cy < lo[1]) lo[1] = p->cy; if (p->cx > hi[0]) hi[0] = p->cx; if (p->cy > hi[1]) hi[1] = p->cy;
            float d = p->cx * p->cx + p->cy * p->cy;
            int at;
            if (kept < 8) at = kept++;
            else if (d < best[7]) at = 7;
            else continue;
            best[at] = d; order[at] = q;
            for (int k = at; k > 0 && best[k] < best[k - 1]; k--) { float t = best[k]; best[k] = best[k - 1]; best[k - 1] = t; int o = order[k]; order[k] = order[k - 1]; order[k - 1] = o; }
        }
        char line[512]; int n = 0;
        for (int k = 0; k < kept && n < 480; k++)
        {
            const NearPad* p = &g_pads.pad[order[k]];
            n += snprintf(line + n, sizeof line - n, " (%.0f, %.0f) r %.0f%s;", (double)p->cx, (double)p->cy, (double)p->a, p->foot ? " foot" : p->rect ? " rect" : "");
        }
        Note("difficulty: near terrain: pad centres x %.0f..%.0f, y %.0f..%.0f (world size %u); nearest the centre:%s", (double)lo[0], (double)hi[0], (double)lo[1],
            (double)hi[1], size, line);
    }
}

/* W over the tile at NEAR_PAD_STEP, the largest pad weight at each point (made once per tile: a mid-mission tile has
 * its own centre, and W is laid out around it). */
static void NearPadRaster(void)
{
    if (g_pads.wLevel == g_world.levels && g_pads.wCentre[0] == g_near.centre[0] && g_pads.wCentre[1] == g_near.centre[1]) return;
    g_pads.wLevel = g_world.levels;
    g_pads.wCentre[0] = g_near.centre[0]; g_pads.wCentre[1] = g_near.centre[1];
    if (g_pads.w == NULL) g_pads.w = (float*)VirtualAlloc(NULL, (size_t)NEAR_PAD_N * NEAR_PAD_N * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_pads.w == NULL) return;
    memset(g_pads.w, 0, (size_t)NEAR_PAD_N * NEAR_PAD_N * 4);
    if (g_pads.level != g_world.levels) return;
    const float x0 = g_near.centre[0] - NEAR_SIDE / 2, y0 = g_near.centre[1] + NEAR_SIDE / 2;
    int used = 0;
    for (int q = 0; q < g_pads.count; q++)
    {
        const NearPad* p = &g_pads.pad[q];
        float reach = p->rect ? sqrtf((p->a + p->ba) * (p->a + p->ba) + (p->b + p->bb) * (p->b + p->bb)) : p->a + p->ba;
        int c0 = (int)floorf((p->cx - reach - x0) / NEAR_PAD_STEP), c1 = (int)ceilf((p->cx + reach - x0) / NEAR_PAD_STEP);
        int r0 = (int)floorf((y0 - p->cy - reach) / NEAR_PAD_STEP), r1 = (int)ceilf((y0 - p->cy + reach) / NEAR_PAD_STEP);
        if (c1 < 0 || r1 < 0 || c0 >= NEAR_PAD_N || r0 >= NEAR_PAD_N) continue;
        used++;
        if (c0 < 0) c0 = 0; if (r0 < 0) r0 = 0; if (c1 > NEAR_PAD_N - 1) c1 = NEAR_PAD_N - 1; if (r1 > NEAR_PAD_N - 1) r1 = NEAR_PAD_N - 1;
        for (int r = r0; r <= r1; r++)
            for (int c = c0; c <= c1; c++)
            {
                float dx = x0 + ((float)c + 0.5f) * NEAR_PAD_STEP - p->cx, dy = y0 - ((float)r + 0.5f) * NEAR_PAD_STEP - p->cy, w;
                if (p->rect)
                {
                    float lx = fabsf(dx * p->c + dy * p->s), ly = fabsf(-dx * p->s + dy * p->c);
                    float ta = (lx - p->a) / p->ba, tb = (ly - p->b) / p->bb;
                    ta = ta < 0 ? 0 : ta > 1 ? 1 : ta; tb = tb < 0 ? 0 : tb > 1 ? 1 : tb;
                    w = p->k * (1 - ta * ta * (3 - 2 * ta)) * (1 - tb * tb * (3 - 2 * tb));
                }
                else
                {
                    float t = (sqrtf(dx * dx + dy * dy) - p->a) / p->ba; t = t < 0 ? 0 : t > 1 ? 1 : t;
                    w = p->k * (1 - t * t * (3 - 2 * t));
                }
                float* at = g_pads.w + (size_t)r * NEAR_PAD_N + c;
                if (w > *at) *at = w;
            }
    }
    size_t full = 0, some = 0;
    for (size_t i = 0; i < (size_t)NEAR_PAD_N * NEAR_PAD_N; i++) { if (g_pads.w[i] >= 0.999f) full++; else if (g_pads.w[i] > 0.0f) some++; }
    Note("difficulty: near terrain: the tile takes the level's surface under %d of its pads: %.2f km2 at full weight, %.2f km2 in their bands", used,
        (double)full * NEAR_PAD_STEP * NEAR_PAD_STEP / 1e6, (double)some * NEAR_PAD_STEP * NEAR_PAD_STEP / 1e6);
}

static float NearPadWeight(float wx, float wy)
{
    if (g_pads.w == NULL || g_pads.wLevel != g_world.levels || g_pads.wCentre[0] != g_near.centre[0] || g_pads.wCentre[1] != g_near.centre[1]) return 0.0f;
    float u = (wx - (g_near.centre[0] - NEAR_SIDE / 2)) / NEAR_PAD_STEP - 0.5f, v = ((g_near.centre[1] + NEAR_SIDE / 2) - wy) / NEAR_PAD_STEP - 0.5f;
    int c = (int)floorf(u), r = (int)floorf(v);
    if (c < 0 || r < 0 || c >= NEAR_PAD_N - 1 || r >= NEAR_PAD_N - 1) return 0.0f;
    float fu = u - (float)c, fv = v - (float)r;
    const float* w = g_pads.w + (size_t)r * NEAR_PAD_N + c;
    return (w[0] * (1 - fu) + w[1] * fu) * (1 - fv) + (w[NEAR_PAD_N] * (1 - fu) + w[NEAR_PAD_N + 1] * fu) * fv;
}

/* The target at a tile texel (t its height, low the lowpass of C - T there, coarse C there): inside the hidden square
 * the coarse surface at its edge and the tile's own shape from 160 m in; past it the coarse surface for NEAR_BAND_FLAT,
 * then NearBand under it. */
static float NearTarget(float wx, float wy, float t, float low, float coarse)
{
    float e = NearOutside(wx, wy);
    if (e > 0) return NearCoarseTri(wx, wy) - NearBand(e); /* the drawn triangles (equal to the bilinear on the edge line) */
    float u = (-e - 16.0f) / 144.0f; u = u < 0 ? 0 : u > 1 ? 1 : u;
    float target = coarse + u * u * (3 - 2 * u) * (t + low - coarse);
    float w = NearPadWeight(wx, wy); /* under the level's pads: the level's surface */
    return target + w * (coarse - target);
}

/* The target and the grid's values (H - T at each grid vertex). */
/* HELLFORGE_XL_NEAR_DUMP=1: the tile as rendered, before the conform (near-tile-pre.u16, u16 as the texture). */
static void NearDumpPre(void)
{
    char t[4] = "";
    if (!(GetEnvironmentVariableA("HELLFORGE_XL_NEAR_DUMP", t, sizeof t) > 0 && t[0] == '1')) return;
    uint16_t* u = (uint16_t*)VirtualAlloc(NULL, (size_t)4096 * 4096 * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (u == NULL) return;
    for (size_t i = 0; i < (size_t)4096 * 4096; i++) { float v = g_near.heights[i] * 65535.0f + 0.5f; u[i] = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v); }
    /* a tile built in mission: its own file, named by its centre (metres) */
    wchar_t name[64];
    if (g_moveBuilding) swprintf(name, 64, L"near-tile-pre-%d-%d.u16", (int)lroundf(g_near.centre[0]), (int)lroundf(g_near.centre[1]));
    else wcscpy(name, L"near-tile-pre.u16");
    HANDLE f = OpenLog(name);
    if (f != INVALID_HANDLE_VALUE) { DWORD wrote; WriteFile(f, u, 4096u * 4096u * 2u, &wrote, NULL); CloseHandle(f); }
    VirtualFree(u, 0, MEM_RELEASE);
}

static double g_conformPlanMean, g_conformPlanWorst;
static int NearConformPlan(void)
{
    LARGE_INTEGER t0, t1, hz; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&hz);
    const size_t n = (size_t)4096 * 4096;
    if (g_conform.low == NULL) g_conform.low = (float*)VirtualAlloc(NULL, n * 4 * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_conform.delta == NULL) g_conform.delta = (float*)VirtualAlloc(NULL, (size_t)NEAR_GRID * NEAR_GRID * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_conform.low == NULL || g_conform.delta == NULL) return 0;
    NearPadRaster(); /* W: the level's pads over the tile (NearTarget) */
    float* a = g_conform.low, *b = g_conform.low + n;
    const float x0 = g_near.centre[0] - NEAR_SIDE / 2 + NEAR_TEXEL / 2, y0 = g_near.centre[1] + NEAR_SIDE / 2 - NEAR_TEXEL / 2;
    /* the low-frequency mismatch: lowpass(C - T), C bilinear at every tile texel */
    for (int r = 0; r < 4096; r++)
        for (int c = 0; c < 4096; c++)
        {
            size_t at = (size_t)r * 4096 + c;
            a[at] = NearCoarseAt(x0 + NEAR_TEXEL * (float)c, y0 - NEAR_TEXEL * (float)r) - (g_near.heights[at] * 1000.0f - 500.0f);
        }
    for (int pass = 0; pass < 3; pass++) { NearBox(a, b, NEAR_SHIFT_RADIUS, 0); NearBox(b, a, NEAR_SHIFT_RADIUS, 1); }
    NearDumpPre();
    /* the strip around the hidden square's edge (NearFineCell's cells and a texel more): H - T at every texel, in b
     * (the box passes' scratch, free now) */
    for (int r = 0; r < 4096; r++)
        for (int c = 0; c < 4096; c++)
        {
            float wx = x0 + NEAR_TEXEL * (float)c, wy = y0 - NEAR_TEXEL * (float)r;
            if (fabsf(NearOutside(wx, wy)) > NEAR_FINE + 3.0f) continue;
            size_t at = (size_t)r * 4096 + c;
            float t = g_near.heights[at] * 1000.0f - 500.0f;
            b[at] = NearTarget(wx, wy, t, a[at], NearCoarseAt(wx, wy)) - t;
        }
    g_conform.ring = b;
    double worst = 0, sum = 0;
    for (int j = 0; j < NEAR_GRID; j++)
        for (int i = 0; i < NEAR_GRID; i++)
        {
            float tc = NearGridTexel(i), tr = NearGridTexel(j);
            int c = tc < 0 ? 0 : tc > 4095 ? 4095 : (int)tc, r = tr < 0 ? 0 : tr > 4095 ? 4095 : (int)tr;
            size_t at = (size_t)r * 4096 + c;
            float t = g_near.heights[at] * 1000.0f - 500.0f;
            float wx = x0 + NEAR_TEXEL * (float)c, wy = y0 - NEAR_TEXEL * (float)r;
            /* inside: metres in from the tile's edge */
            float coarse = NearCoarseAt(wx, wy), target = NearTarget(wx, wy, t, a[at], coarse);
            float d = target - t;
            if (d > 480.0f) d = 480.0f; if (d < -480.0f) d = -480.0f;
            g_conform.delta[(size_t)j * NEAR_GRID + i] = d;
            if (fabs(d) > worst) worst = fabs(d);
            sum += fabs(d);
        }
    /* blocks where the target is the lowpass alone: every texel of the block 160 m or more inside the hidden square's
     * edge (NearTarget's u = 1, with a texel's margin) and no pad weight on any pad cell the block's bilinear W reads */
    g_conform.allBlocks = 0; g_conform.blocksCoarse = 0;
    if (g_conform.block == NULL) g_conform.block = (uint8_t*)VirtualAlloc(NULL, (size_t)NEAR_BLOCKS * NEAR_BLOCKS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_conform.block != NULL)
        for (int bj = 0; bj < NEAR_BLOCKS; bj++)
            for (int bi = 0; bi < NEAR_BLOCKS; bi++)
            {
                float tx0 = NearGridTexel(2 * bi), tx1 = NearGridTexel(2 * bi + 2), ty0 = NearGridTexel(2 * bj), ty1 = NearGridTexel(2 * bj + 2);
                float wxa = x0 + NEAR_TEXEL * tx0, wxb = x0 + NEAR_TEXEL * tx1, wya = y0 - NEAR_TEXEL * ty0, wyb = y0 - NEAR_TEXEL * ty1;
                int ok = NearOutside(wxa, wya) <= -160.0f - NEAR_TEXEL && NearOutside(wxb, wya) <= -160.0f - NEAR_TEXEL &&
                         NearOutside(wxa, wyb) <= -160.0f - NEAR_TEXEL && NearOutside(wxb, wyb) <= -160.0f - NEAR_TEXEL;
                if (ok && g_pads.w != NULL && g_pads.wLevel == g_world.levels && g_pads.wCentre[0] == g_near.centre[0] && g_pads.wCentre[1] == g_near.centre[1])
                {
                    /* NearPadWeight's cells: u = (wx - west) / step - 0.5, cells floor(u) and floor(u) + 1 */
                    int c0 = (int)floorf((wxa - (g_near.centre[0] - NEAR_SIDE / 2)) / NEAR_PAD_STEP - 0.5f), c1 = (int)floorf((wxb - (g_near.centre[0] - NEAR_SIDE / 2)) / NEAR_PAD_STEP - 0.5f) + 1;
                    int r0 = (int)floorf(((g_near.centre[1] + NEAR_SIDE / 2) - wya) / NEAR_PAD_STEP - 0.5f), r1 = (int)floorf(((g_near.centre[1] + NEAR_SIDE / 2) - wyb) / NEAR_PAD_STEP - 0.5f) + 1;
                    for (int r = r0 < 0 ? 0 : r0; ok && r <= r1 && r < NEAR_PAD_N; r++)
                        for (int c = c0 < 0 ? 0 : c0; c <= c1 && c < NEAR_PAD_N; c++)
                            if (g_pads.w[(size_t)r * NEAR_PAD_N + c] != 0.0f) { ok = 0; break; }
                }
                g_conform.block[(size_t)bj * NEAR_BLOCKS + bi] = (uint8_t)ok;
                g_conform.blocksCoarse += (uint32_t)ok;
            }
    QueryPerformanceCounter(&t1);
    g_conform.ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart;
    g_conformPlanMean = sum / ((double)NEAR_GRID * NEAR_GRID); g_conformPlanWorst = worst;
    return 1;
}

/* The plan on a thread of its own (s72m: 679 ms, which on the game thread was one frame that long). It reads and writes
 * only the conform's buffers, g_near's heights and centre and the level's pads, which nothing touches while the conform
 * waits for it (phase 9). */
static DWORD WINAPI NearConformPlanThread(LPVOID unused)
{
    g_conform.planOk = NearConformPlan();
    InterlockedExchange(&g_conform.planDone, 1);
    InterlockedExchange(&g_conform.planRunning, 0);
    return 0;
}

/* A job on the generator's queue that draws its gui (the triangles put there since the last one) into the merge's
 * heights, as the game queues its own (entry 0x1D0 bytes: world, camera, generator, R+0x108, settings+0x74). */
static void NearGuiJob(uint64_t queue, uint64_t world, uint64_t camera, uint64_t generator, uint64_t r108, uint64_t settings)
{
    volatile LONG64* ticket = (volatile LONG64*)(uintptr_t)queue;
    LONG64 mine = InterlockedExchangeAdd64(ticket, 1);
    while (*(volatile LONG64*)(uintptr_t)(queue + 8) != mine) YieldProcessor();
    uint32_t n = *(volatile uint32_t*)(uintptr_t)(queue + 0x10);
    uint8_t* e = (uint8_t*)(uintptr_t)(queue + 0x18 + (uint64_t)n * 0x1D0);
    *(volatile uint32_t*)(uintptr_t)(queue + 0x10) = n + 1;
    memcpy(e + 0x00, &world, 8); memcpy(e + 0x08, &camera, 8); memcpy(e + 0x10, &generator, 8); memcpy(e + 0x18, &r108, 8);
    memset(e + 0x20, 0, 4); e[0x24] = 0; memset(e + 0x38, 0, 2);
    SafeRead(settings + 0x74, e + 0x58, 16);
    { uint32_t id = 0xFFFFFFFFu; memcpy(e + 0x68, &id, 4); }
    InterlockedExchangeAdd64((volatile LONG64*)(uintptr_t)(queue + 8), 1);
}

/* ---- The coarse terrain's ring: its samples on the hidden square's edge, NEAR_RING_DIP lower ----------------------
 * Drawn at the level's own finalize (0x64, held until the job is done), as the conform draws on the tile: two gui
 * triangles a rectangle with the merge's Voronoi material add their vertex z to the heights 0x64 converts, sharp to the
 * texel. The level's merge draws in level pixels with the identity transform (the replay's wrapper is not on yet):
 * texel (c, r) is the pixel square [c, c + 1] x [N - 1 - r, N - r] (camera over +-size/2, row 0 at +y). Corners once.
 * The loader's own copies of the level's heights (g_near.global, the conform's readback) get the ring put back
 * (NearRingRestore); the game's copy keeps it, as drawn (the visible surface, NearVisibleAt). */
/* The grid (HELLFORGE_XL_GRID_RING=0: the drop tile's square alone): the tiles built in mission (difficulty_world_move.c)
 * stand on cells of 512 samples from the drop tile's square, (a, b) with b north; a cell is one whose square lies inside
 * the samples [1, 4094] (NearHideCoarse). The ring is every cell's edge: column lines c0 + 512 a for a in [a0, a1 + 1]
 * over the rows [rMin, rMax], row lines r0 - 512 b for b in [b0 - 1, b1] over the columns [cMin, cMax]. A dip of
 * NEAR_RING_DIP on lines across the coarse terrain where no tile stands tilts its normals by atan(dip / 8 m), 1.8 deg at
 * the default 0.25 m (the PS's central difference over two 4 m samples). */
static struct { LONG level; int phase, applied; uint32_t waits; float centre[2]; int c0, c1, r0, r1; int a0, a1, b0, b1, cMin, cMax, rMin, rMax; } g_ring = {.level = -1};

static void NearRingGrid(void)
{
    char t[4] = "";
    int grid = !(GetEnvironmentVariableA("HELLFORGE_XL_GRID_RING", t, sizeof t) > 0 && t[0] == '0');
    g_ring.a0 = g_ring.a1 = g_ring.b0 = g_ring.b1 = 0;
    if (grid)
    {
        /* c0 + 512 a in [1, 3583]; r0 - 512 b in [1, 3583] */
        g_ring.a0 = (int)ceilf((1.0f - g_ring.c0) / 512.0f); g_ring.a1 = (int)floorf((3583.0f - g_ring.c0) / 512.0f);
        g_ring.b0 = (int)ceilf((g_ring.r0 - 3583.0f) / 512.0f); g_ring.b1 = (int)floorf((g_ring.r0 - 1.0f) / 512.0f);
        if (g_ring.a0 > 0 || g_ring.a1 < 0 || g_ring.b0 > 0 || g_ring.b1 < 0) g_ring.a0 = g_ring.a1 = g_ring.b0 = g_ring.b1 = 0;
    }
    g_ring.cMin = g_ring.c0 + 512 * g_ring.a0; g_ring.cMax = g_ring.c0 + 512 * (g_ring.a1 + 1);
    g_ring.rMin = g_ring.r0 - 512 * g_ring.b1; g_ring.rMax = g_ring.r0 - 512 * (g_ring.b0 - 1);
}

static int NearRingDraw(uint64_t context)
{
    typedef uint64_t (*MatFn)(uint64_t gui, uint64_t hash);
    typedef void (*TriFn)(uint64_t gui, const float* p0, const float* p1, const float* p2, uint32_t layer, const float* colour, uint64_t material,
        const float* uv0, const float* uv1, const float* uv2);
    MatFn mat = (MatFn)NearApi(0xD0, 0x230);
    TriFn tri = (TriFn)NearApi(0xD0, 0x0);
    uintptr_t gen = 0, lg = 0;
    uint64_t R = 0, gui = 0, world = 0, settings = 0, queue = 0, camera = 0, generator = 0, r108 = 0;
    float samples = 0; /* lg + 0x120: the level's samples a side, a float (4096.0) */
    float colour[4] = {0};
    if (mat == NULL || tri == NULL || SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0 || (lg = gen + 0x8758A8u, 0) || SafeRead(gen, &R, 8) || R == 0 ||
        SafeRead(R + 0x110, &gui, 8) || gui == 0 || SafeRead(context + 0x40, &world, 8) || world == 0 || SafeRead(context + 8, &settings, 8) || settings == 0 ||
        SafeRead(lg + 0x120, &samples, 4) || samples != 4096.0f || SafeRead(context + 0x50, &queue, 8) || queue == 0 || SafeRead(R, &camera, 8) ||
        SafeRead(R + 0x28, &generator, 8) || SafeRead(R + 0x108, &r108, 8) || SafeRead(g_world.game + 0x23C8540u, colour, 16)) return 0;
    uint64_t material = mat(gui, 0x13DD2DCAD2968560ull);
    const float N = samples, z = -NEAR_RING_DIP;
    /* x0, x1, y0, y1 in pixels: each column line whole, each row line between two column lines (a texel once) */
    int rects = 0;
    for (int a = g_ring.a0; a <= g_ring.a1 + 1; a++)
    {
        float c = (float)(g_ring.c0 + 512 * a);
        float rect[4] = {c, c + 1, N - 1 - (float)g_ring.rMax, N - (float)g_ring.rMin};
        float v[4][3] = {{rect[0], rect[2], z}, {rect[1], rect[2], z}, {rect[1], rect[3], z}, {rect[0], rect[3], z}};
        tri(gui, v[0], v[1], v[2], 0, colour, material, NULL, NULL, NULL);
        tri(gui, v[0], v[2], v[3], 0, colour, material, NULL, NULL, NULL);
        rects++;
    }
    for (int b = g_ring.b0 - 1; b <= g_ring.b1; b++)
        for (int a = g_ring.a0; a <= g_ring.a1; a++)
        {
            float r = (float)(g_ring.r0 - 512 * b), c = (float)(g_ring.c0 + 512 * a);
            float rect[4] = {c + 1, c + 512, N - 1 - r, N - r};
            float v[4][3] = {{rect[0], rect[2], z}, {rect[1], rect[2], z}, {rect[1], rect[3], z}, {rect[0], rect[3], z}};
            tri(gui, v[0], v[1], v[2], 0, colour, material, NULL, NULL, NULL);
            tri(gui, v[0], v[2], v[3], 0, colour, material, NULL, NULL, NULL);
            rects++;
        }
    g_ring.waits = (uint32_t)rects; /* for the note; NearRingStep counts its waits from 0 again */
    NearGuiJob(queue, world, camera, generator, r108, settings);
    return 1;
}

/* The level's 0x64: 1 while the ring is drawn (the record waits), 0 to let it run. */
static int NearRingStep(uint64_t context)
{
    if (g_ring.phase == 0)
    {
        g_ring.applied = 0;
        if (!NearChooseCentre(g_ring.centre, 0))
        {
            g_ring.level = g_world.levels;
            Note("difficulty: near terrain: no drop point before the level's finalize; the coarse terrain's ring is not lowered");
            return 0;
        }
        g_ring.c0 = (int)floorf((g_ring.centre[0] - NEAR_HIDE / 2 + 8191.75f) / NEAR_DC + 0.5f); g_ring.c1 = g_ring.c0 + 512;
        int b0 = (int)floorf((g_ring.centre[1] - NEAR_HIDE / 2 + 8191.75f) / NEAR_DC + 0.5f);
        g_ring.r1 = 4095 - b0; g_ring.r0 = g_ring.r1 - 512;
        NearRingGrid();
        if (!NearRingDraw(context))
        {
            g_ring.level = g_world.levels;
            Note("difficulty: near terrain: the level's merge is not there at its finalize; the coarse terrain's ring is not lowered");
            return 0;
        }
        Note("difficulty: near terrain: the coarse terrain's ring: %u rectangles, cells %d..%d east, %d..%d north of the drop tile's (columns %d..%d, rows %d..%d)",
            g_ring.waits, g_ring.a0, g_ring.a1, g_ring.b0, g_ring.b1, g_ring.cMin, g_ring.cMax, g_ring.rMin, g_ring.rMax);
        g_ring.phase = 1; g_ring.waits = 0;
        return 1;
    }
    uint32_t q = NearQueued(context);
    if (q != 0 && q != 0xFFFFFFFFu && ++g_ring.waits < 1200) return 1;
    g_ring.applied = q == 0; g_ring.phase = 0; g_ring.level = g_world.levels;
    Note("difficulty: near terrain: the coarse terrain's samples on the hidden square's edge (columns %d, %d, rows %d, %d) %s %.2f m lower",
        g_ring.c0, g_ring.c1, g_ring.r0, g_ring.r1, g_ring.applied ? "drawn" : "NOT drawn (the queue did not empty)", (double)NEAR_RING_DIP);
    return 0;
}

/* The loader's copies of the level's heights: the ring's samples back up by the dip, when the copy holds it (the mean
 * of ring - (inside + outside) / 2 over the ring, against -dip / 2). fetch/store in metres. */
static void NearRingRestore(const char* what, float (*fetch)(void*, int, int), void (*store)(void*, int, int, float), void* copy)
{
    if (g_ring.level != g_world.levels || !g_ring.applied || copy == NULL) return;
    double sum = 0; int n = 0;
    for (int k = 1; k < 512; k++)
    {
        int pts[4][6] = {{g_ring.c0, g_ring.r0 + k, g_ring.c0 + 1, g_ring.r0 + k, g_ring.c0 - 1, g_ring.r0 + k},
                         {g_ring.c1, g_ring.r0 + k, g_ring.c1 - 1, g_ring.r0 + k, g_ring.c1 + 1, g_ring.r0 + k},
                         {g_ring.c0 + k, g_ring.r0, g_ring.c0 + k, g_ring.r0 + 1, g_ring.c0 + k, g_ring.r0 - 1},
                         {g_ring.c0 + k, g_ring.r1, g_ring.c0 + k, g_ring.r1 - 1, g_ring.c0 + k, g_ring.r1 + 1}};
        for (int q = 0; q < 4; q++)
        { sum += fetch(copy, pts[q][0], pts[q][1]) - 0.5 * (fetch(copy, pts[q][2], pts[q][3]) + fetch(copy, pts[q][4], pts[q][5])); n++; }
    }
    double mean = n ? sum / n : 0;
    int dipped = mean < -NEAR_RING_DIP / 2;
    if (dipped)
        for (int r = g_ring.rMin; r <= g_ring.rMax; r++)
        {
            if ((((r - g_ring.r0) % 512 + 512) % 512) == 0) { for (int c = g_ring.cMin; c <= g_ring.cMax; c++) store(copy, c, r, fetch(copy, c, r) + NEAR_RING_DIP); continue; }
            for (int c = g_ring.c0 + 512 * g_ring.a0; c <= g_ring.cMax; c += 512) store(copy, c, r, fetch(copy, c, r) + NEAR_RING_DIP);
        }
    Note("difficulty: near terrain: %s: the ring stands %.2f m from its neighbours' mean%s", what, mean, dipped ? "; put back up for the loader's use" : "");
}

static float NearFetchU16(void* p, int c, int r) { return ((uint16_t*)p)[(size_t)r * 4096 + c] / 65535.0f * 1000.0f - 500.0f; }
static void NearStoreU16(void* p, int c, int r, float m) { float v = (m + 500.0f) / 1000.0f * 65535.0f + 0.5f; ((uint16_t*)p)[(size_t)r * 4096 + c] = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v); }
static float NearFetchF32(void* p, int c, int r) { return ((float*)p)[(size_t)r * 4096 + c]; }
static void NearStoreF32(void* p, int c, int r, float m) { ((float*)p)[(size_t)r * 4096 + c] = m; }

/* One batch from g_conform.next (blocks of 2 x 2 grid cells, numbered row by row) and its job. A block is two triangles
 * on its corners (4.5 m) where the drawn value is smooth over it: the base everywhere (a bilinear surface on the
 * capture's 16 m cells; the triangles' deviation from it is at most ~t h^2 / 1024 for a cell twist t, and h = 4.5 m
 * keeps it within cm), the conform where its target is the lowpass alone (160 m and more inside the hidden square's
 * edge and no pad weight over the block: the mismatch lowpass(C - T), sigma ~64 m, interpolated within ~A / 2000).
 * Elsewhere each of its four cells is drawn as before (2 triangles, or a texel a cell in the ring). Loading-screen
 * frames pace the batches (one a frame), so fewer triangles is fewer frames (hellforge-1a, NEAR_TILE.md levers 1/2:
 * base 128 -> 32 batches, conform ~205 -> ~139). */
static uint32_t NearCellCost(int i, int j) { return g_conform.ring != NULL && NearFineCell(i, j) ? 50 : 2; }
static int NearConformDraw(uint64_t context)
{
    typedef uint64_t (*MatFn)(uint64_t gui, uint64_t hash);
    typedef void (*TriFn)(uint64_t gui, const float* p0, const float* p1, const float* p2, uint32_t layer, const float* colour, uint64_t material,
        const float* uv0, const float* uv1, const float* uv2);
    MatFn mat = (MatFn)NearApi(0xD0, 0x230);
    TriFn tri = (TriFn)NearApi(0xD0, 0x0);
    float colour[4] = {0};
    uint64_t queue = 0, camera = 0, generator = 0, r108 = 0;
    if (mat == NULL || tri == NULL || SafeRead(g_world.game + 0x23C8540u, colour, 16) || SafeRead(context + 0x50, &queue, 8) || queue == 0 ||
        SafeRead(g_near.R, &camera, 8) || SafeRead(g_near.R + 0x28, &generator, 8) || SafeRead(g_near.R + 0x108, &r108, 8)) return 0;
    uint64_t material = mat(g_near.gui, 0x13DD2DCAD2968560ull);
    /* level pixels (the replay's gui transform takes them to the tile): x = (world + size/2) x samples / size, y up */
    const float px = g_near.samples / g_near.world_size, half = g_near.world_size / 2;
    const float x0 = g_near.centre[0] - NEAR_SIDE / 2 + NEAR_TEXEL / 2, y0 = g_near.centre[1] + NEAR_SIDE / 2 - NEAR_TEXEL / 2;
    const uint32_t blocks = NEAR_BLOCKS * NEAR_BLOCKS;
    uint32_t end = g_conform.next, tris = 0;
    for (; end < blocks; end++)
    {
        int bi = (int)(end % NEAR_BLOCKS), bj = (int)(end / NEAR_BLOCKS);
        uint32_t cost = g_conform.allBlocks || (g_conform.block != NULL && g_conform.block[end]) ? 2 :
            NearCellCost(2 * bi, 2 * bj) + NearCellCost(2 * bi + 1, 2 * bj) + NearCellCost(2 * bi, 2 * bj + 1) + NearCellCost(2 * bi + 1, 2 * bj + 1);
        if (tris + cost > NEAR_BATCH) break;
        tris += cost;
    }
    for (uint32_t kb = g_conform.next; kb < end; kb++)
    {
        int bi = (int)(kb % NEAR_BLOCKS), bj = (int)(kb / NEAR_BLOCKS);
        if (g_conform.allBlocks || (g_conform.block != NULL && g_conform.block[kb]))
        {
            float v[4][3];
            for (int q = 0; q < 4; q++)
            {
                int gi = 2 * bi + 2 * (q == 1 || q == 2), gj = 2 * bj + 2 * (q >= 2);
                v[q][0] = (x0 + NEAR_TEXEL * NearGridTexel(gi) + half) * px;
                v[q][1] = (y0 - NEAR_TEXEL * NearGridTexel(gj) + half) * px;
                v[q][2] = g_conform.delta[(size_t)gj * NEAR_GRID + gi];
            }
            tri(g_near.gui, v[0], v[1], v[2], 0, colour, material, NULL, NULL, NULL);
            tri(g_near.gui, v[0], v[2], v[3], 0, colour, material, NULL, NULL, NULL);
            continue;
        }
        for (int cell = 0; cell < 4; cell++)
        {
        int i = 2 * bi + (cell & 1), j = 2 * bj + (cell >> 1);
        if (g_conform.ring != NULL && NearFineCell(i, j))
        {
            /* a texel a cell: lines at the cell's own edges and every texel centre between them */
            float us[6], vs[6]; int nu = 0, nv = 0;
            float u0 = NearGridTexel(i), u1 = NearGridTexel(i + 1), w0 = NearGridTexel(j), w1 = NearGridTexel(j + 1);
            us[nu++] = u0; for (int t = (int)floorf(u0) + 1; (float)t < u1; t++) us[nu++] = (float)t; us[nu++] = u1;
            vs[nv++] = w0; for (int t = (int)floorf(w0) + 1; (float)t < w1; t++) vs[nv++] = (float)t; vs[nv++] = w1;
            for (int b = 0; b + 1 < nv; b++)
                for (int a = 0; a + 1 < nu; a++)
                {
                    float v[4][3];
                    for (int q = 0; q < 4; q++)
                    {
                        float tu = us[a + (q == 1 || q == 2)], tv = vs[b + (q >= 2)];
                        int c = tu < 0 ? 0 : tu > 4095 ? 4095 : (int)tu, r = tv < 0 ? 0 : tv > 4095 ? 4095 : (int)tv;
                        float d = g_conform.ring[(size_t)r * 4096 + c];
                        if (d > 480.0f) d = 480.0f; if (d < -480.0f) d = -480.0f;
                        v[q][0] = (x0 + NEAR_TEXEL * tu + half) * px;
                        v[q][1] = (y0 - NEAR_TEXEL * tv + half) * px;
                        v[q][2] = d;
                    }
                    tri(g_near.gui, v[0], v[1], v[2], 0, colour, material, NULL, NULL, NULL);
                    tri(g_near.gui, v[0], v[2], v[3], 0, colour, material, NULL, NULL, NULL);
                }
            continue;
        }
        float v[4][3];
        for (int q = 0; q < 4; q++)
        {
            int gi = i + (q == 1 || q == 2), gj = j + (q >= 2);
            v[q][0] = (x0 + NEAR_TEXEL * NearGridTexel(gi) + half) * px;
            v[q][1] = (y0 - NEAR_TEXEL * NearGridTexel(gj) + half) * px;
            v[q][2] = g_conform.delta[(size_t)gj * NEAR_GRID + gi];
        }
        tri(g_near.gui, v[0], v[1], v[2], 0, colour, material, NULL, NULL, NULL);
        tri(g_near.gui, v[0], v[2], v[3], 0, colour, material, NULL, NULL, NULL);
        }
    }
    g_conform.next = end;
    NearGuiJob(queue, g_near.world, camera, generator, r108, g_near.settings);
    return 1;
}

/* ---- The tile's base: the level's own smoothed heights (HELLFORGE_XL_NEAR_BASE=0: the level's 0x3C record instead) ----
 * Replaying the level's RenderHeightmapVoronoi record drew the level's ~300 m cells at the tile's 0.5 m texels: their hard
 * features (caps; 8.0 m steps along straight lines, run v22 step dump after the record: single-texel jumps of 8.03, 8.06,
 * 8.07 m) were made for the level's 4 m texels, where 0x40's blur (fixed in texels) widens them to 110-280 m; on the tile
 * the same blur leaves 16-20 m cliffs. The tile starts instead from the level's heights right after its own 0x40 (the
 * level's base, its composite tiles and the level-scale smoothing): the first intermediate capture, which the loader keeps
 * at the level's 0x3E (g_near.caps / capData, descriptor 0 = mip 0: 1024^2 R32F, terrain_generator_capture_intermediate_
 * heightmap = DOWNSAMPLE_4x4 of generated_heightmap_f, so heightmap_f metres, 16 m a texel; texel (c, r) covers generator
 * metres [16c, 16c + 16] x and, row 0 at +y as the sampler 0x31C8B0 reads it (row = (1 - v)(h - 1)), world y
 * [8192 - 16(r + 1), 8192 - 16r]). It is drawn bilinear on the conform's 2 m grid (NearConformDraw: a triangle with the
 * Voronoi material adds its vertex z exactly) onto the target 0x39 cleared; the tile's composite tiles then add the
 * vanilla-density cells, and the later passes (pads, zone heights, stamps) work on the level's heights as on the level. */
static int NearBaseOn(void)
{
    static int on = -1;
    if (on < 0) { char t[4] = ""; on = !(GetEnvironmentVariableA("HELLFORGE_XL_NEAR_BASE", t, sizeof t) > 0 && t[0] == '0'); }
    return on;
}

static int NearBasePlan(void)
{
    uint32_t w = 0, hgt = 0, format = 0;
    memcpy(&w, g_near.caps, 4); memcpy(&hgt, g_near.caps + 4, 4); memcpy(&format, g_near.caps + 8, 4);
    const float* L = (const float*)g_near.capData[0];
    if (L == NULL || w != 1024 || hgt != 1024 || NearTexelBytes(format) != 4 || g_near.capBytes[0] < (size_t)1024 * 1024 * 4)
    { Note("difficulty: near terrain: base: the level's capture is %u x %u format %u; no tile", w, hgt, format); return 0; }
    if (g_conform.delta == NULL) g_conform.delta = (float*)VirtualAlloc(NULL, (size_t)NEAR_GRID * NEAR_GRID * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_conform.delta == NULL) return 0;
    g_conform.ring = NULL; g_conform.allBlocks = 1; /* the base: 4.5 m blocks everywhere */
    const float x0 = g_near.centre[0] - NEAR_SIDE / 2 + NEAR_TEXEL / 2, y0 = g_near.centre[1] + NEAR_SIDE / 2 - NEAR_TEXEL / 2;
    float lo = 1e9f, hi = -1e9f; double sum = 0;
    for (int j = 0; j < NEAR_GRID; j++)
        for (int i = 0; i < NEAR_GRID; i++)
        {
            float wx = x0 + NEAR_TEXEL * NearGridTexel(i), wy = y0 - NEAR_TEXEL * NearGridTexel(j);
            float fx = (wx + 8192.0f) / 16.0f - 0.5f, fy = (8192.0f - wy) / 16.0f - 0.5f;
            if (fx < 0) fx = 0; if (fy < 0) fy = 0; if (fx > 1022.999f) fx = 1022.999f; if (fy > 1022.999f) fy = 1022.999f;
            int c = (int)fx, r = (int)fy;
            float ax = fx - c, ay = fy - r;
            const float* r0 = L + (size_t)r * 1024, *r1 = r0 + 1024;
            float v = (r0[c] * (1 - ax) + r0[c + 1] * ax) * (1 - ay) + (r1[c] * (1 - ax) + r1[c + 1] * ax) * ay;
            if (!isfinite(v)) v = 0;
            g_conform.delta[(size_t)j * NEAR_GRID + i] = v;
            if (v < lo) lo = v; if (v > hi) hi = v; sum += v;
        }
    Note("difficulty: near terrain: base: the level's smoothed heights (its first capture, 16 m) drawn as the tile's base: %.1f..%.1f m, mean %.1f",
        (double)lo, (double)hi, sum / ((double)NEAR_GRID * NEAR_GRID));
    return 1;
}

/* NearRunStep for the level's 0x3C record: 0 while drawing, 1 when drawn, -1 on failure. */
static int NearBaseStep(uint64_t context)
{
    if (g_near.calls == 0)
    {
        if (!NearBasePlan()) return -1;
        g_conform.next = 0; g_conform.waits = 0; g_near.calls = 1;
        return 0;
    }
    if (NearQueued(context) != 0) return ++g_conform.waits < 1200 ? 0 : -1;
    g_conform.waits = 0;
    if (g_conform.next >= (uint32_t)NEAR_BLOCKS * NEAR_BLOCKS) { g_near.calls = 0; return 1; }
    if (!NearConformDraw(context)) return -1;
    return 0;
}

/* The conform's stages; 1 while running, 0 when done (ok or not: the tile goes on with what the texture holds). */
/* ---- The tile's material map near its edge ----------------------------------------------------------------------
 * The tile's map (0.5625 m texels, painted from its own heights) and the coarse terrain's (4 m) differ in detail, so
 * where the coarse terrain takes over a straight material line ran along every edge (s70t..s70w along-views). A texel
 * is self-contained (HS 0754b766 unpacks ubfe(5,5,5,8) at 26/21/16/8: three ids and a value; copying is valid,
 * blending is not), so near the edge the tile takes the coarse terrain's texels: from NEAR_MAT_EDGE inside the hidden
 * square outward every texel, and inward a share that falls off with a smoothstep over NEAR_MAT_BAND, whose inner
 * boundary wanders by NEAR_MAT_WOBBLE (value noise, 96 m). Which texels switch follows a coherent noise (blobs of a
 * few metres, its histogram flattened so the share is the smoothstep), and each switched texel takes one of the four
 * coarse texels around it with the bilinear weights, picked by two more coherent noises (2.5 m): the coarse terrain's
 * own 4 m interpolation as patches, not 4 m squares. Offline check on s70w's maps: scratchpad matband/proto.py. */
#define NEAR_MAT_BAND (g_xp.matBand)
#define NEAR_MAT_WOBBLE (g_xp.matWobble)
#define NEAR_MAT_EDGE (g_xp.matEdge)

static float NearHash(int32_t a, int32_t b, uint32_t seed)
{
    uint32_t v = (uint32_t)a * 374761393u + (uint32_t)b * 668265263u + seed * 1442695041u;
    v = (v ^ (v >> 13)) * 1274126177u;
    return (float)((v ^ (v >> 16)) & 0xFFFFu) / 65535.0f;
}

static float NearValueNoise(float x, float y, float scale, uint32_t seed)
{
    float gx = x / scale, gy = y / scale, fx0 = floorf(gx), fy0 = floorf(gy), fx = gx - fx0, fy = gy - fy0;
    int32_t ix = (int32_t)fx0, iy = (int32_t)fy0;
    float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    float a = NearHash(ix, iy, seed) * (1 - sx) + NearHash(ix + 1, iy, seed) * sx;
    float b = NearHash(ix, iy + 1, seed) * (1 - sx) + NearHash(ix + 1, iy + 1, seed) * sx;
    return a * (1 - sy) + b * sy;
}

static float NearFbm(float x, float y, float scale, uint32_t seed, int octaves)
{
    float sum = 0, amp = 1, total = 0;
    for (int o = 0; o < octaves; o++) { sum += amp * NearValueNoise(x, y, scale, seed + (uint32_t)o); total += amp; amp *= 0.5f; scale *= 0.5f; }
    return sum / total;
}

#define NEAR_MAT_BINS 1024
typedef struct { float cdf[3][NEAR_MAT_BINS + 1]; } NearMatCdf;

static float NearMatUniform(const NearMatCdf* t, int k, float v)
{
    float f = v * NEAR_MAT_BINS; int i = (int)f;
    if (i < 0) return 0.0f;
    if (i >= NEAR_MAT_BINS) return 1.0f;
    return t->cdf[k][i] + (f - i) * (t->cdf[k][i + 1] - t->cdf[k][i]);
}

/* ---- The level's material map, interpolated (extra large levels) -------------------------------------------------------
 * The level's generated_materials (4096^2 over the 16 km world, 4 m a texel) is a 2 x 2 upsample of the 2048^2
 * imp_weight_buffers (8 m), and the terrain PS sharpens each material pair to a threshold (soft zone dw ~0.1), so every
 * material border is a right-angle staircase of 8 m steps (auditor round 6, Goal 2 FAIL). Texel layout (static,
 * matblend review 2026-11-03): ids at bits 26 / 21 / 16 (A, B, C, ascending, id 0 = unused), w_B bits 8-15, w_C bits
 * 0-7, w_A = 1 - w_B - w_C; bit 31 = hole. The VS `ld`s per vertex, the HS unions the 12 slots of a patch, the DS
 * interpolates the weights, the PS takes the top 3. So the map itself must carry the smooth borders: each texel here is
 * the bilinear mix of the native texels' per-id weights at its own position, domain-warped by value noise (8 m, 40 m
 * and 12 m) so the borders wander like the detailed generator's; the 3 largest kept, renormalised, packed as the game's.
 * Reference implementation: matblend/process.py (offline, checked on the s71c dumps). One function for any world point,
 * so the coarse map and the tile's band (NearMatBand) agree where they meet. */
#define NEAR_MAT_STEP (16383.5 / 4095.0)
static double NearMatHash(int64_t ix, int64_t iy, uint32_t seed)
{
    uint32_t h = (uint32_t)ix * 374761393u + (uint32_t)iy * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (double)((h ^ (h >> 16)) & 0xFFFFFFu) / 16777216.0;
}

static double NearMatNoise(double x, double y, double wl, uint32_t seed) /* [-1, 1] */
{
    double u = x / wl, v = y / wl, i = floor(u), j = floor(v), fu = u - i, fv = v - j;
    fu = fu * fu * (3 - 2 * fu); fv = fv * fv * (3 - 2 * fv);
    int64_t a = (int64_t)i, b = (int64_t)j;
    return ((NearMatHash(a, b, seed) * (1 - fu) + NearMatHash(a + 1, b, seed) * fu) * (1 - fv) +
        (NearMatHash(a, b + 1, seed) * (1 - fu) + NearMatHash(a + 1, b + 1, seed) * fu) * fv) * 2 - 1;
}

#define NEAR_MAT_WARP 8.0
#define NEAR_MAT_WARP_WL 40.0
/* src: the level's map (4096^2, row 0 at +y; native texel (I, J) = src[2I][2J]); (x, y) world metres. */
/* The native texels' per-id weights mixed bilinearly at (x, y) (centred metres, y north), domain-warped; returns the
 * number of ids in id[] / w[] (at most 12); *tx_ / *ty_ get the native texel coordinates (for the hole bit). */
static int NearMatMix(const uint32_t* src, double x, double y, uint32_t* id, double* w, double* tx_, double* ty_)
{
    double wx = NEAR_MAT_WARP * (0.7 * NearMatNoise(x, y, NEAR_MAT_WARP_WL, 11) + 0.3 * NearMatNoise(x, y, NEAR_MAT_WARP_WL / 3.3, 12));
    double wy = NEAR_MAT_WARP * (0.7 * NearMatNoise(x, y, NEAR_MAT_WARP_WL, 21) + 0.3 * NearMatNoise(x, y, NEAR_MAT_WARP_WL / 3.3, 22));
    double j = (x + 8191.75) / NEAR_MAT_STEP, i = (8191.75 - y) / NEAR_MAT_STEP;
    double tx = (j - 0.5) / 2 + wx / (2 * NEAR_MAT_STEP), ty = (i - 0.5) / 2 - wy / (2 * NEAR_MAT_STEP); /* +y north = -row */
    tx = tx < 0 ? 0 : tx > 2047 ? 2047 : tx; ty = ty < 0 ? 0 : ty > 2047 ? 2047 : ty;
    int J0 = (int)floor(tx), I0 = (int)floor(ty);
    if (J0 > 2046) J0 = 2046;
    if (I0 > 2046) I0 = 2046;
    double fx = tx - J0, fy = ty - I0;
    int n = 0;
    for (int k = 0; k < 4; k++)
    {
        int dI = k >> 1, dJ = k & 1;
        double beta = (dJ ? fx : 1 - fx) * (dI ? fy : 1 - fy);
        uint32_t t = src[(size_t)(2 * (I0 + dI)) * 4096 + (size_t)(2 * (J0 + dJ))];
        uint32_t ids[3] = {(t >> 26) & 31, (t >> 21) & 31, (t >> 16) & 31};
        double wb = ((t >> 8) & 255) / 255.0, wc = (t & 255) / 255.0, ws[3] = {1 - wb - wc, wb, wc};
        for (int s = 0; s < 3; s++)
        {
            if (ids[s] == 0) continue;
            int q = 0;
            while (q < n && id[q] != ids[s]) q++;
            if (q == n) { id[n] = ids[s]; w[n] = 0; n++; }
            w[q] += beta * ws[s];
        }
    }
    *tx_ = tx; *ty_ = ty;
    return n;
}

static void NearRoadStampAt(const uint32_t* src, double x, double y, uint32_t* id, double* w, int* n);

static uint32_t NearMatPack(uint32_t* id, double* w, int n, uint32_t hole);
static uint32_t NearMatEncodeAt(const uint32_t* src, double x, double y)
{
    uint32_t id[13]; double w[13], tx, ty;
    int n = NearMatMix(src, x, y, id, w, &tx, &ty);
    NearRoadStampAt(src, x, y, id, w, &n);
    int hc = (int)floor(tx + 0.5), hr = (int)floor(ty + 0.5);
    uint32_t hole = src[(size_t)(2 * (hr > 2047 ? 2047 : hr)) * 4096 + (size_t)(2 * (hc > 2047 ? 2047 : hc))] & 0x80000000u;
    return NearMatPack(id, w, n, hole);
}

/* per-id weights -> a texel: the 3 largest, ids ascending, the hole bit as given */
static uint32_t NearMatPack(uint32_t* id, double* w, int n, uint32_t hole)
{
    /* the 3 largest (ties, within rounding: the smaller id), those under half a step dropped, renormalised */
    uint32_t tid[3] = {0, 0, 0}; double tw[3] = {0, 0, 0}, sum = 0;
    for (int r = 0; r < 3; r++)
    {
        int best = -1;
        for (int q = 0; q < n; q++)
            if (id[q] != 0 && (best < 0 || w[q] * 64 - id[q] * 1e-6 > w[best] * 64 - id[best] * 1e-6)) best = q;
        if (best < 0) break;
        if (w[best] > 0.5 / 255) { tid[r] = id[best]; tw[r] = w[best]; sum += w[best]; }
        id[best] = 0;
    }
    if (sum > 1e-9) for (int r = 0; r < 3; r++) tw[r] /= sum;
    /* ids ascending, unused (0) last */
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 2 - a; b++)
        {
            uint32_t ka = tid[b] ? tid[b] : 99, kb = tid[b + 1] ? tid[b + 1] : 99;
            if (ka > kb) { uint32_t ti = tid[b]; tid[b] = tid[b + 1]; tid[b + 1] = ti; double tt = tw[b]; tw[b] = tw[b + 1]; tw[b + 1] = tt; }
        }
    uint32_t vB = (uint32_t)floor(tw[1] * 255 + 0.5), vC = (uint32_t)floor(tw[2] * 255 + 0.5);
    if (vB + vC > 255) vC = 255 - vB;
    return (tid[0] << 26) | (tid[1] << 21) | (tid[2] << 16) | (vB << 8) | vC | hole;
}

/* Rows of a job over the CPU's cores (the caller's thread takes part); returns when every row is done. */
typedef void (*NearRowsFn)(void* ctx, int r0, int r1);
typedef struct { NearRowsFn fn; void* ctx; volatile LONG next; int rows; } NearRowsJob;
static DWORD WINAPI NearRowsWorker(LPVOID p)
{
    NearRowsJob* job = (NearRowsJob*)p;
    for (;;)
    {
        LONG r = InterlockedExchangeAdd(&job->next, 16);
        if (r >= job->rows) break;
        job->fn(job->ctx, (int)r, (int)(r + 16 < job->rows ? r + 16 : job->rows));
    }
    return 0;
}

static void NearRows(NearRowsFn fn, void* ctx, int rows)
{
    NearRowsJob job = {fn, ctx, 0, rows};
    SYSTEM_INFO si; GetSystemInfo(&si);
    int want = (int)si.dwNumberOfProcessors - 1;
    want = want < 0 ? 0 : want > 15 ? 15 : want;
    HANDLE t[15]; int made = 0;
    for (int k = 0; k < want; k++) { t[made] = CreateThread(NULL, 256 * 1024, NearRowsWorker, &job, 0, NULL); if (t[made] != NULL) made++; }
    NearRowsWorker(&job);
    if (made > 0) WaitForMultipleObjects((DWORD)made, t, TRUE, INFINITE);
    for (int k = 0; k < made; k++) CloseHandle(t[k]);
}

/* The coarse terrain's map: made at the conform (the level's map read back), bound in its spawn's material slot
 * (NearOneMipSpawn), served to that spawn's physics readback (NearMatReadAsync) and copied over the scatter's CPU copy
 * lg+0xF0 (op 0x66's readback descriptor {w, h, format, ?, data}: game 0xC17A9F-0xC17AB5; the data is the allocator
 * 0x1A10200's heap block, exe 0x31B616-0x31B631, so it is written in place). Off with HELLFORGE_XL_COARSE_MAT=0. */

static int NearCoarseMatOn(void)
{
    static int on = -1;
    if (on < 0) { char t[4] = ""; on = !(GetEnvironmentVariableA("HELLFORGE_XL_COARSE_MAT", t, sizeof t) > 0 && t[0] == '0'); }
    return on;
}

typedef struct { const uint32_t* src; uint32_t* out; } NearEncodeJob;
static void NearEncodeRows(void* ctx, int r0, int r1)
{
    NearEncodeJob* job = (NearEncodeJob*)ctx;
    for (int r = r0; r < r1; r++)
        for (int c = 0; c < 4096; c++)
            job->out[(size_t)r * 4096 + c] = NearMatEncodeAt(job->src, -8191.75 + c * NEAR_MAT_STEP, 8191.75 - r * NEAR_MAT_STEP);
}

/* The share of coarse texels at (wx, wy): 1 from NEAR_MAT_EDGE inside the hidden square outward, 0 past the band. */
static float NearMatShare(float wx, float wy)
{
    float e = NearOutside(wx, wy);
    if (e >= -NEAR_MAT_EDGE) return 1.0f;
    if (e < -NEAR_MAT_EDGE - NEAR_MAT_BAND - NEAR_MAT_WOBBLE) return 0.0f;
    float inner = -NEAR_MAT_EDGE - NEAR_MAT_BAND - NEAR_MAT_WOBBLE * NearFbm(wx, wy, 96.0f, 11u, 4);
    float u = (e - inner) / (-NEAR_MAT_EDGE - inner);
    if (u <= 0) return 0.0f;
    u = u > 1 ? 1 : u;
    return u * u * (3 - 2 * u);
}

/* tile: the tile's map (4096^2, row 0 at +y), changed in place; coarse: the level's (4096^2 over the world). The band's
 * texels take the level's map interpolated at their own point (NearMatEncodeAt: what the coarse terrain draws there),
 * dithered against the tile's own by the share. Returns the number of texels taken. */
/* Paths in the coarse terrain's material map from their vector data (audit 11: straight grey strips with 8 m steps
 * across the open land).
 * Op 0x41's setup (0xF4F9E0) has the base zone type's painters (list +0x70) draw, with their own "material", a quad
 * along every route edge flagged 0x100 (0xF51C20) straight from the edge's position +0 to +8 (corner metres), half width
 * [edge+0x30] x 0.5 (x 0.333 when R+0x938 != 0; R = [settings+0xB0]) (0xF51C2A..0xF51CE0), and an octagon at every route
 * node (G + g_routeNodes, 0x98 each, count G+0x11A71B4; position +0, 16 edge pointers +0x10) of radius 0.42 x the
 * widest incident edge (0xF52500..0xF52588, 0x23C6A24). They land in the 2048^2 weight buffers: 8 m texels on a 16 km
 * map, so a 2-12 m path (s75l: 2386 flagged edges, 2.0..12.0 m, mean 6.2; R+0x938 0) became a staircase of 8 m texels.
 * The path's id is the painters' data, so it is measured: along the flagged edges, every 8 m, the native ids at the
 * centreline and 48 m to either side; the id with the largest (centre - side) share is the path (needs centre >= 0.2
 * and 3 x its share aside, else nothing is stamped; s75m: id 19, 0.33 vs 0.06 -- paths under a texel's width often
 * leave no texel). In NearMatEncodeAt, within 16 m of a path the native path id is
 * removed and the path is drawn from the vectors with a 4 m box filter (the texel); where the native map holds the
 * path id at the nearest centreline point only (so water or later painters that cover a path keep it covered); and
 * where removing it leaves no land, the land is taken from 20 m beyond the corridor. */
#define ROAD_CELL 64.0
#define ROAD_CELLS 256
#define ROAD_CORRIDOR 16.0
#define ROAD_FILTER 4.0
typedef struct { float ax, ay, bx, by, hw; } RoadSeg; /* centred metres; a node is a = b */
static struct { int on; uint32_t id; LONG level; uint32_t count; RoadSeg* seg; uint32_t* start; uint32_t* list; } g_road = {.level = -1};

static void NearRoadStampDrop(void)
{
    if (g_road.seg) VirtualFree(g_road.seg, 0, MEM_RELEASE);
    if (g_road.start) VirtualFree(g_road.start, 0, MEM_RELEASE);
    if (g_road.list) VirtualFree(g_road.list, 0, MEM_RELEASE);
    g_road.seg = NULL; g_road.start = NULL; g_road.list = NULL; g_road.on = 0; g_road.count = 0; g_road.level = -1;
}

/* signed distance from (x, y) to the path set, and the nearest centreline point */
static double NearRoadDistance(double x, double y, double* px, double* py)
{
    int cx = (int)floor((x + 8192.0) / ROAD_CELL), cy = (int)floor((y + 8192.0) / ROAD_CELL);
    if (cx < 0 || cy < 0 || cx >= ROAD_CELLS || cy >= ROAD_CELLS) return 1e9;
    uint32_t c = (uint32_t)cy * ROAD_CELLS + cx;
    double best = 1e9;
    for (uint32_t k = g_road.start[c]; k < g_road.start[c + 1]; k++)
    {
        const RoadSeg* r = &g_road.seg[g_road.list[k]];
        double dx = r->bx - r->ax, dy = r->by - r->ay, l2 = dx * dx + dy * dy, t = 0;
        if (l2 > 1e-6) { t = ((x - r->ax) * dx + (y - r->ay) * dy) / l2; t = t < 0 ? 0 : t > 1 ? 1 : t; }
        double qx = r->ax + t * dx, qy = r->ay + t * dy, d = sqrt((x - qx) * (x - qx) + (y - qy) * (y - qy)) - r->hw;
        if (d < best) { best = d; *px = qx; *py = qy; }
    }
    return best;
}

static double NearRoadWeightOf(const uint32_t* id, const double* w, int n, uint32_t which)
{
    for (int q = 0; q < n; q++) if (id[q] == which) return w[q];
    return 0;
}

/* A source's per-id weights at a world point: the coarse map (NearMatMix, warped) or a tile's own map. */
typedef int (*NearMixFn)(const void* ctx, double x, double y, uint32_t* id, double* w);
static int NearMixCoarse(const void* ctx, double x, double y, uint32_t* id, double* w)
{
    double tx, ty;
    return NearMatMix((const uint32_t*)ctx, x, y, id, w, &tx, &ty);
}

/* The path drawn over id[] / w[] at (x, y): filter = the box filter's width (the texel the result stands for). */
static void NearRoadStampWith(NearMixFn mix, const void* ctx, double filter, double x, double y, uint32_t* id, double* w, int* n)
{
    if (!g_road.on || g_road.level != g_world.levels) return;
    double qx = 0, qy = 0, d = NearRoadDistance(x, y, &qx, &qy);
    if (d >= ROAD_CORRIDOR) return;
    uint32_t cid[13]; double cw[13];
    int cn = mix(ctx, qx, qy, cid, cw);
    if (NearRoadWeightOf(cid, cw, cn, g_road.id) < 0.25) return; /* the game left no path here (covered, or too thin) */
    double c = 0.5 - d / filter;
    c = c < 0 ? 0 : c > 1 ? 1 : c;
    double land = 0;
    for (int q = 0; q < *n; q++) { if (id[q] == g_road.id) w[q] = 0; land += w[q]; }
    if (land < 0.2)
    {
        /* all path natively: the land from beyond the corridor, away from the centreline */
        double ux = x - qx, uy = y - qy, l = sqrt(ux * ux + uy * uy);
        if (l < 1e-3) { ux = 1; uy = 0; l = 1; }
        double out = ROAD_CORRIDOR + 20.0 - d;
        *n = mix(ctx, x + ux / l * out, y + uy / l * out, id, w);
        land = 0;
        for (int q = 0; q < *n; q++) { if (id[q] == g_road.id) w[q] = 0; land += w[q]; }
        if (land < 1e-6) return;
    }
    for (int q = 0; q < *n; q++) w[q] *= (1 - c) / land;
    int q = 0;
    while (q < *n && id[q] != g_road.id) q++;
    if (q == *n) { if (*n >= 13) return; id[q] = g_road.id; w[q] = 0; (*n)++; }
    w[q] = c;
}

static void NearRoadStampAt(const uint32_t* src, double x, double y, uint32_t* id, double* w, int* n)
{
    NearRoadStampWith(NearMixCoarse, src, ROAD_FILTER, x, y, id, w, n);
}

/* A tile's own map (4096^2 over NEAR_SIDE, texel (r, c) at x0 + c t, y0 - r t; a 2 x 2 upsample of its 2048^2 weight
 * buffers, ~1.1 m): the native texels' per-id weights bilinearly at a world point, unwarped. */
typedef struct { const uint32_t* tile; double x0, y0; } NearTileSrc;
static int NearMixTile(const void* ctx, double x, double y, uint32_t* id, double* w)
{
    const NearTileSrc* t = (const NearTileSrc*)ctx;
    double fc = (x - t->x0) / NEAR_TEXEL, fr = (t->y0 - y) / NEAR_TEXEL;
    double tx = (fc - 0.5) / 2, ty = (fr - 0.5) / 2;
    tx = tx < 0 ? 0 : tx > 2047 ? 2047 : tx; ty = ty < 0 ? 0 : ty > 2047 ? 2047 : ty;
    int J0 = (int)floor(tx), I0 = (int)floor(ty);
    if (J0 > 2046) J0 = 2046;
    if (I0 > 2046) I0 = 2046;
    double fx = tx - J0, fy = ty - I0;
    int n = 0;
    for (int k = 0; k < 4; k++)
    {
        int dI = k >> 1, dJ = k & 1;
        double beta = (dJ ? fx : 1 - fx) * (dI ? fy : 1 - fy);
        uint32_t v = t->tile[(size_t)(2 * (I0 + dI)) * 4096 + (size_t)(2 * (J0 + dJ))];
        uint32_t ids[3] = {(v >> 26) & 31, (v >> 21) & 31, (v >> 16) & 31};
        double wb = ((v >> 8) & 255) / 255.0, wc = (v & 255) / 255.0, ws[3] = {1 - wb - wc, wb, wc};
        for (int s3 = 0; s3 < 3; s3++)
        {
            if (ids[s3] == 0) continue;
            int q = 0;
            while (q < n && id[q] != ids[s3]) q++;
            if (q == n) { id[n] = ids[s3]; w[n] = 0; n++; }
            w[q] += beta * ws[s3];
        }
    }
    return n;
}

typedef struct { uint32_t* tile; const uint32_t* coarse; const NearMatCdf* cdf; int reach, encoded; float x0, y0; volatile LONG taken; const uint32_t* own; volatile LONG paths; } NearBandJob;
#define NEAR_BAND_SCALE 6.0f
#define NEAR_BAND_SEED 23u
#define NEAR_BAND_OCT 3
static void NearMatBandRows(void* ctx, int r0, int r1)
{
    NearBandJob* job = (NearBandJob*)ctx;
    LONG taken = 0, paths = 0;
    NearTileSrc own = {job->own, job->x0, job->y0};
    for (int r = r0; r < r1; r++)
        for (int c = 0; c < 4096; c++)
        {
            int interior = r >= job->reach && r < 4096 - job->reach && c >= job->reach && c < 4096 - job->reach;
            if (interior && job->own == NULL) { c = 4096 - job->reach - 1; continue; }
            float wx = job->x0 + NEAR_TEXEL * c, wy = job->y0 - NEAR_TEXEL * r;
            float share = interior ? 0 : NearMatShare(wx, wy);
            if (share <= 0 || (share < 1 && NearMatUniform(job->cdf, 0, NearFbm(wx, wy, NEAR_BAND_SCALE, NEAR_BAND_SEED, NEAR_BAND_OCT)) >= share))
            {
                /* the tile's own texel: the bilinear mix of its native 2048^2 weight texels (~1.1 m) at the texel's own
                 * position, and its paths from their vectors. The replay's map is a 2 x 2 upsample of those weights and the
                 * terrain PS reads 4 texels and thresholds the blend (pixel shader 364ba1ef: alpha = sat(5(2g(a) + h - 2.5)
                 * + 0.5)), with one mip on XL, so every patch border drew as a staircase of 1.125 m steps (audit 12
                 * criterion 8; static, subagent surfacing). Mixed, the blend varies smoothly and the cut is a curve. */
                if (job->own == NULL) continue;
                uint32_t id[13]; double w[13];
                int n = NearMixTile(&own, wx, wy, id, w);
                double qx, qy;
                if (g_road.on && g_road.level == g_world.levels && NearRoadDistance(wx, wy, &qx, &qy) < ROAD_CORRIDOR)
                { NearRoadStampWith(NearMixTile, &own, 1.0, wx, wy, id, w, &n); paths++; }
                job->tile[(size_t)r * 4096 + c] = NearMatPack(id, w, n, job->own[(size_t)r * 4096 + c] & 0x80000000u);
                continue;
            }
            if (job->encoded) job->tile[(size_t)r * 4096 + c] = NearMatEncodeAt(job->coarse, wx, wy);
            else /* the coarse terrain draws the level's own map: its nearest texel */
            {
                int ic = (int)floorf((wx + 8191.75f) / (float)NEAR_MAT_STEP + 0.5f), ir = (int)floorf((8191.75f - wy) / (float)NEAR_MAT_STEP + 0.5f);
                ic = ic < 0 ? 0 : ic > 4095 ? 4095 : ic; ir = ir < 0 ? 0 : ir > 4095 ? 4095 : ir;
                job->tile[(size_t)r * 4096 + c] = job->coarse[(size_t)ir * 4096 + ic];
            }
            taken++;
        }
    InterlockedExchangeAdd(&job->taken, taken);
    InterlockedExchangeAdd(&job->paths, paths);
}

static uint32_t NearMatBand(uint32_t* tile, const uint32_t* coarse)
{
    static NearMatCdf cdf;
    NearBandJob job = {tile, coarse, &cdf, 0, g_coarseMat.tex != 0 && g_coarseMat.level == g_world.levels, g_near.centre[0] - NEAR_SIDE / 2 + NEAR_TEXEL / 2,
        g_near.centre[1] + NEAR_SIDE / 2 - NEAR_TEXEL / 2, 0};
    job.reach = (int)((NEAR_SIDE - NEAR_HIDE) / 2 / NEAR_TEXEL + (NEAR_MAT_EDGE + NEAR_MAT_BAND + NEAR_MAT_WOBBLE) / NEAR_TEXEL) + 2;
    /* the dither noise's histogram over the band (every 5th texel), flattened to uniform */
    static uint32_t hist[NEAR_MAT_BINS];
    memset(hist, 0, sizeof hist);
    uint32_t samples = 0;
    for (int r = 0; r < 4096; r += 5)
        for (int c = 0; c < 4096; c += 5)
        {
            if (r >= job.reach && r < 4096 - job.reach && c >= job.reach && c < 4096 - job.reach) continue;
            float wx = job.x0 + NEAR_TEXEL * c, wy = job.y0 - NEAR_TEXEL * r;
            int b = (int)(NearFbm(wx, wy, NEAR_BAND_SCALE, NEAR_BAND_SEED, NEAR_BAND_OCT) * NEAR_MAT_BINS);
            hist[b < 0 ? 0 : b >= NEAR_MAT_BINS ? NEAR_MAT_BINS - 1 : b]++;
            samples++;
        }
    uint32_t run = 0;
    cdf.cdf[0][0] = 0.0f;
    for (int b = 0; b < NEAR_MAT_BINS; b++) { run += hist[b]; cdf.cdf[0][b + 1] = samples ? (float)run / (float)samples : (float)(b + 1) / NEAR_MAT_BINS; }
    /* the tile's own map as the replay painted it, for its paths (read while the rows write the tile) */
    uint32_t* own = NULL;
    {
        own = (uint32_t*)VirtualAlloc(NULL, (size_t)4096 * 4096 * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (own != NULL) memcpy(own, tile, (size_t)4096 * 4096 * 4);
    }
    job.own = own;
    NearRows(NearMatBandRows, &job, 4096);
    if (own != NULL)
    {
        VirtualFree(own, 0, MEM_RELEASE);
        Note("difficulty: near terrain: the tile's own map mixed from its weight texels; its paths drawn from their vectors: %ld texels", (long)job.paths);
    }
    return (uint32_t)job.taken;
}

/* ---- The tile's material map with the band, as a texture of its own -------------------------------------------------
 * The band (NearMatBand) is built on the CPU from the read-back maps, so it reaches the GPU as a texture created with
 * data: [api+0x10]+0x6A8 = exe 0x31E740(w, h, engine format, pixels) (static, scratchpad texwrite.md / matspawn.md): a
 * type-0 texture, 1 mip; the pixels are referenced, not copied (the render side copies them when it runs that frame's
 * command 0xE), so they stay allocated until the texture is destroyed (+0x6B0 = 0x31E920, which does not free them).
 * Engine format 1 gives the same R32_TYPELESS resource and R32_FLOAT view as the render target the replay painted
 * (0x6E3DB6 / 0x6E3EB7 vs 0x6EF8C1 / 0x6E8832), and the terrain shaders `ld` the bits at mip 0 (the one shader that
 * reads mips, PS 3e4550e5, clamps to the view's one mip). Nothing in the spawn (0x1AABE0) or the render build tests the
 * type for 0 vs 1; the material_map binding writes the u32 handle at obj+0 (0x2FAE69). The texture is made at the end
 * of the conform and the spawn waits NEAR_MAT_HOLD calls and 100 ms after it, so the render side has made it before
 * the first draw (the order of command 0xE against the spawn's first draw within one frame is not established).
 * The spawn's own CPU copy: it reads the material map back (async call 0x10B200 at exe 0x1AB12C: rcx renderer, rdx
 * the map, r8 the world's allocator world+0x230058, r9 &block+0x48, [rsp+0x20] &unit+0x51 done, [rsp+0x28] u16 mip),
 * and that path (0x6CF4B0 / 0x6CFB00) knows render targets only. For the tile's spawn call that call goes to
 * NearMatReadback, which gives the physics the band's pixels the way the readback would: a tight 4096 x 4096 x 4 image
 * from the same allocator (vt+0x30(self, &out, bytes, 8), as 0x6CF603; the teardown 0x1ADC20 frees it with vt+0x40),
 * *r9 = it, done = 1. The sync site (0x1AB157 -> 0x10B0D0, taken when [rbp+0x1568] is 0; InitSpawnTerrain passes 1)
 * reads back the tile's render target instead. Off with HELLFORGE_XL_NEAR_MATBAND=0. */
#define NEAR_MAT_HOLD 3
#define NEAR_MAT_ASYNC_SITE 0x1AB12Cu
#define NEAR_MAT_SYNC_SITE 0x1AB157u
static struct { uint64_t tex; uint32_t* pixels; ULONGLONG made; int holds; uint32_t taken; uint8_t* stub; int served; } g_matband;
static struct { LONG level; uint32_t* pixels; } g_levelMat = {.level = -1}; /* the level's material map as read at its conform */

static int NearMatBandOn(void)
{
    static int on = -1;
    if (on < 0) { char t[4] = ""; on = !(GetEnvironmentVariableA("HELLFORGE_XL_NEAR_MATBAND", t, sizeof t) > 0 && t[0] == '0'); }
    return on;
}

static void NearWriteMap(const wchar_t* name, const uint8_t* data, size_t bytes)
{
    HANDLE f = OpenLog(name);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    for (size_t at = 0; at < bytes; at += 4096 * 4096) WriteFile(f, data + at, (DWORD)(bytes - at < 4096 * 4096 ? bytes - at : 4096 * 4096), &wrote, NULL);
    CloseHandle(f);
    Note("difficulty: near terrain: %ls written (HELLFORGE_XL_NEAR_DUMP)", name);
}

/* The band's texture and pixels gone (a new level: the world that drew them is gone; or the band was not made). */
static void NearMatDrop(void)
{
    if (g_matband.tex != 0)
    {
        typedef void (*DestroyFn)(uint64_t tex);
        DestroyFn destroy = (DestroyFn)NearApi(0x10, 0x6B0);
        if (destroy != NULL) destroy(g_matband.tex);
        else { g_matband.tex = 0; g_matband.pixels = NULL; return; } /* cannot destroy it: the pixels stay with it */
        g_matband.tex = 0;
    }
    if (g_matband.pixels != NULL) VirtualFree(g_matband.pixels, 0, MEM_RELEASE);
    g_matband.pixels = NULL;
}

static void NearMatTexture(ULONGLONG t0);
static void NearMatBuild(const uint32_t* coarse)
{
    ULONGLONG t0 = GetTickCount64();
    g_matband.taken = NearMatBand(g_matband.pixels, coarse);
    NearMatTexture(t0);
}

/* The band's texture from g_matband.pixels (the render API: the game thread). */
static void NearMatTexture(ULONGLONG t0)
{
    typedef uint64_t (*CreateFn)(uint32_t w, uint32_t h, uint32_t format, void* pixels);
    CreateFn create = (CreateFn)NearApi(0x10, 0x6A8);
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    g_matband.tex = (uintptr_t)create == exe + 0x31E740u && ApiCodeMatches(exe + 0x31E740u, "48 89 5C 24 08 48 89 6C 24 10 56 57 41 54 41 56 41 57 48 83 EC 50") &&
        (uintptr_t)NearApi(0x10, 0x6B0) == exe + 0x31E920u ? create(4096, 4096, 1, g_matband.pixels) : 0;
    g_matband.made = GetTickCount64(); g_matband.holds = 0; g_matband.served = 0;
    if (g_matband.tex == 0) { Note("difficulty: near terrain: the material band's texture was not created; the tile keeps its own map to the edge"); return; }
    Note("difficulty: near terrain: material band: %u texels of the tile's map take the coarse terrain's (from %.0f m inside the hidden square's edge, "
        "+-%.0f m), %.2f s; texture 0x%llX", g_matband.taken, (double)(NEAR_MAT_EDGE + NEAR_MAT_BAND), (double)(NEAR_MAT_WOBBLE / 2), (double)(GetTickCount64() - t0) / 1000.0,
        (unsigned long long)g_matband.tex);
}

/* The coarse map's texture and pixels gone (a new level, or it was not made). */
static void NearCoarseMatDrop(void)
{
    if (g_coarseMat.tex != 0)
    {
        typedef void (*DestroyFn)(uint64_t tex);
        DestroyFn destroy = (DestroyFn)NearApi(0x10, 0x6B0);
        if (destroy == NULL) { g_coarseMat.tex = 0; g_coarseMat.pixels = NULL; g_coarseMat.level = -1; return; } /* the pixels stay with it */
        destroy(g_coarseMat.tex);
        g_coarseMat.tex = 0;
    }
    if (g_coarseMat.pixels != NULL) VirtualFree(g_coarseMat.pixels, 0, MEM_RELEASE);
    g_coarseMat.pixels = NULL; g_coarseMat.level = -1; g_coarseMat.slot = NULL;
}

/* level: the level's map as read back (4096^2): the path set and its id, before the coarse map is encoded. */
static void NearRoadStampBuild(const uint32_t* level)
{
    NearRoadStampDrop();
    uintptr_t gen = 0, region = 0;
    uint32_t edges = 0, nodes = 0, world = 0; float narrow = 0;
    if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0 || SafeRead(gen + 0x11A71CCu, &edges, 4) || SafeRead(gen + 0x11A71B4u, &nodes, 4) ||
        SafeRead(g_worldLevel + 0x2D20 + 0x2C, &world, 4) || world != 16384 || edges > 65536 || nodes > 65536) return;
    if (!SafeRead(g_worldLevel + 0x2D20 + 0xB0, &region, 8) && region != 0) SafeRead(region + 0x938, &narrow, 4);
    double scale = narrow != 0.0f ? 0.333 : 1.0;
    g_road.seg = (RoadSeg*)VirtualAlloc(NULL, sizeof(RoadSeg) * (edges + nodes + 1), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_road.seg == NULL) return;
    uint32_t m = 0, flagged = 0, discs = 0;
    for (uint32_t e = 0; e < edges; e++)
    {
        uint8_t edge[0x48];
        if (SafeRead(gen + g_routeEdges + 0x48ull * e, edge, sizeof edge)) break;
        float p[4], wd; uint16_t flags;
        memcpy(p, edge, 16); memcpy(&wd, edge + 0x30, 4); memcpy(&flags, edge + 0x44, 2);
        if (!(flags & 0x100) || !(wd > 0)) continue;
        double dx = p[2] - p[0], dy = p[3] - p[1];
        if (dx * dx + dy * dy < 1e-6) continue;
        g_road.seg[m++] = (RoadSeg){p[0] - 8192.0f, p[1] - 8192.0f, p[2] - 8192.0f, p[3] - 8192.0f, (float)(0.5 * wd * scale)};
        flagged++;
    }
    for (uint32_t v = 0; v < nodes; v++)
    {
        uint8_t node[0x98];
        if (SafeRead(gen + g_routeNodes + 0x98ull * v, node, sizeof node)) break;
        float pos[2], widest = 0; memcpy(pos, node, 8);
        for (int k = 0; k < 16; k++)
        {
            uint64_t e = 0; float wd = 0;
            memcpy(&e, node + 0x10 + 8 * k, 8);
            if (e == 0) break;
            if (!SafeRead(e + 0x30, &wd, 4) && wd > widest) widest = wd;
        }
        if (!(widest > 0)) continue;
        g_road.seg[m++] = (RoadSeg){pos[0] - 8192.0f, pos[1] - 8192.0f, pos[0] - 8192.0f, pos[1] - 8192.0f, 0.42f * widest};
        discs++;
    }
    g_road.count = m;
    /* the id: centre vs 48 m to either side, every 8 m along the flagged edges */
    static uint32_t centre[32], side[32];
    memset(centre, 0, sizeof centre); memset(side, 0, sizeof side);
    uint32_t samples = 0;
    for (uint32_t k = 0; k < flagged; k++)
    {
        const RoadSeg* r = &g_road.seg[k];
        double dx = r->bx - r->ax, dy = r->by - r->ay, len = sqrt(dx * dx + dy * dy), nx = -dy / len, ny = dx / len;
        for (double u = 4.0; u < len; u += 8.0)
        {
            double x = r->ax + dx * u / len, y = r->ay + dy * u / len;
            for (int o = -1; o <= 1; o++)
            {
                double px = x + o * 48.0 * nx, py = y + o * 48.0 * ny;
                int col = (int)floor((px + 8192.0) / 4.0), row = (int)floor((8192.0 - py) / 4.0);
                if (col < 0 || col > 4095 || row < 0 || row > 4095) continue;
                uint32_t t = level[(size_t)row * 4096 + col];
                double wb = ((t >> 8) & 255) / 255.0, wc = (t & 255) / 255.0, ws[3] = {1 - wb - wc, wb, wc};
                uint32_t ids[3] = {(t >> 26) & 31, (t >> 21) & 31, (t >> 16) & 31};
                for (int s3 = 0; s3 < 3; s3++) if (ids[s3] && ws[s3] >= 0.25) (o == 0 ? centre : side)[ids[s3]]++;
            }
            samples++;
        }
    }
    uint32_t best = 0; double lead = -1, cShare = 0, sShare = 0;
    for (uint32_t k = 1; k < 32 && samples; k++)
    {
        double cs = (double)centre[k] / samples, ss = (double)side[k] / (2.0 * samples);
        if (cs - ss > lead) { lead = cs - ss; best = k; cShare = cs; sShare = ss; }
    }
    /* the cell lists */
    g_road.start = (uint32_t*)VirtualAlloc(NULL, 4ull * (ROAD_CELLS * ROAD_CELLS + 1), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint32_t total = 0;
    for (int pass = 0; pass < 2 && g_road.start; pass++)
    {
        if (pass == 1)
        {
            uint32_t run = 0;
            for (uint32_t c = 0; c <= ROAD_CELLS * ROAD_CELLS; c++) { uint32_t k = g_road.start[c]; g_road.start[c] = run; run += k; }
            total = g_road.start[ROAD_CELLS * ROAD_CELLS];
            g_road.list = (uint32_t*)VirtualAlloc(NULL, 4ull * (total + 1), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (g_road.list == NULL) break;
        }
        static uint32_t fill[ROAD_CELLS * ROAD_CELLS];
        if (pass == 1) memcpy(fill, g_road.start, sizeof fill);
        for (uint32_t k = 0; k < m; k++)
        {
            const RoadSeg* r = &g_road.seg[k];
            double pad = r->hw + ROAD_CORRIDOR;
            int x0 = (int)floor(((r->ax < r->bx ? r->ax : r->bx) - pad + 8192.0) / ROAD_CELL), x1 = (int)floor(((r->ax > r->bx ? r->ax : r->bx) + pad + 8192.0) / ROAD_CELL);
            int y0 = (int)floor(((r->ay < r->by ? r->ay : r->by) - pad + 8192.0) / ROAD_CELL), y1 = (int)floor(((r->ay > r->by ? r->ay : r->by) + pad + 8192.0) / ROAD_CELL);
            x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0; x1 = x1 >= ROAD_CELLS ? ROAD_CELLS - 1 : x1; y1 = y1 >= ROAD_CELLS ? ROAD_CELLS - 1 : y1;
            double dx = r->bx - r->ax, dy = r->by - r->ay, len = sqrt(dx * dx + dy * dy);
            for (int cy = y0; cy <= y1; cy++)
                for (int cx = x0; cx <= x1; cx++)
                {
                    /* only cells within pad + half a cell's diagonal of the segment */
                    double mx = (cx + 0.5) * ROAD_CELL - 8192.0, my = (cy + 0.5) * ROAD_CELL - 8192.0, t = 0;
                    if (len > 1e-3) { t = ((mx - r->ax) * dx + (my - r->ay) * dy) / (len * len); t = t < 0 ? 0 : t > 1 ? 1 : t; }
                    double ex = mx - (r->ax + t * dx), ey = my - (r->ay + t * dy);
                    if (ex * ex + ey * ey > (pad + ROAD_CELL * 0.7072) * (pad + ROAD_CELL * 0.7072)) continue;
                    uint32_t c = (uint32_t)cy * ROAD_CELLS + cx;
                    if (pass == 0) g_road.start[c]++; else g_road.list[fill[c]++] = k;
                }
        }
    }
    int usable = g_road.start != NULL && g_road.list != NULL && m > 0 && samples > 0 && cShare >= 0.2 && cShare >= 3 * sShare;
    g_road.id = best; g_road.level = g_world.levels; g_road.on = usable && NearCoarseMatOn();
    Note("difficulty: extra large maps: paths: %u flagged route edges (half width 0.5 x w%s) and %u node discs; path id %u under %.2f of the centreline vs %.2f 48 m aside (%u samples); %s",
        flagged, scale < 1 ? " x 0.333" : "", discs, best, cShare, sShare, samples, g_road.on ? "drawn from their vectors in the coarse map" : "not stamped (no clear path id, or the coarse map is off)");
}

/* level: the level's map as read back at the conform (4096^2). */
static void NearCoarseMatBuild(const uint32_t* level)
{
    NearRoadStampBuild(level);
    NearCoarseMatDrop();
    if (!NearCoarseMatOn()) return;
    ULONGLONG t0 = GetTickCount64();
    g_coarseMat.pixels = (uint32_t*)VirtualAlloc(NULL, (size_t)4096 * 4096 * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_coarseMat.pixels == NULL) { Note("difficulty: near terrain: no memory for the coarse terrain's material map; it keeps the level's"); return; }
    NearEncodeJob job = {level, g_coarseMat.pixels};
    NearRows(NearEncodeRows, &job, 4096);
    typedef uint64_t (*CreateFn)(uint32_t w, uint32_t h, uint32_t format, void* pixels);
    CreateFn create = (CreateFn)NearApi(0x10, 0x6A8);
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    g_coarseMat.tex = (uintptr_t)create == exe + 0x31E740u && ApiCodeMatches(exe + 0x31E740u, "48 89 5C 24 08 48 89 6C 24 10 56 57 41 54 41 56 41 57 48 83 EC 50") &&
        (uintptr_t)NearApi(0x10, 0x6B0) == exe + 0x31E920u ? create(4096, 4096, 1, g_coarseMat.pixels) : 0;
    if (g_coarseMat.tex == 0) { Note("difficulty: near terrain: the coarse terrain's material texture was not created; it keeps the level's map"); NearCoarseMatDrop(); return; }
    g_coarseMat.level = g_world.levels; g_coarseMat.made = GetTickCount64(); g_coarseMat.holds = 0; g_coarseMat.served = 0; g_coarseMat.scatter = 0;
    uint32_t changed = 0;
    for (size_t i = 0; i < (size_t)4096 * 4096; i += 7) changed += g_coarseMat.pixels[i] != level[i];
    Note("difficulty: near terrain: the coarse terrain's material map interpolated (warp %.0f m over %.0f m), %.2f s, %.1f%% of texels changed; texture 0x%llX",
        NEAR_MAT_WARP, NEAR_MAT_WARP_WL, (double)(GetTickCount64() - t0) / 1000.0, 100.0 * changed / ((4096.0 * 4096.0 + 6) / 7), (unsigned long long)g_coarseMat.tex);
}

/* After the level's op 0x66: its CPU copy of the material map (lg+0xF0, read by the scatter's brushes and the physics
 * queries) takes the interpolated texels, so grass and rocks follow the borders the coarse terrain draws. */
static void NearCoarseMatScatter(void)
{
    if (g_coarseMat.scatter || g_coarseMat.tex == 0 || g_coarseMat.level != g_world.levels || g_coarseMat.pixels == NULL) return;
    g_coarseMat.scatter = 1;
    uintptr_t gen = 0;
    uint32_t d[4] = {0}; uint64_t data = 0; uint32_t probe = 0;
    if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0 || SafeRead(gen + 0x8758A8u + 0xF0, d, 16) || SafeRead(gen + 0x8758A8u + 0xF0 + 0x10, &data, 8) ||
        data == 0 || d[0] != 4096 || d[1] != 4096 || NearTexelBytes(d[2]) != 4 || SafeRead(data, &probe, 4) || SafeRead(data + (uint64_t)4096 * 4096 * 4 - 4, &probe, 4))
    { Note("difficulty: near terrain: the level's CPU material map is not the reviewed one (%u x %u format %u); its scatter keeps the level's map", d[0], d[1], d[2]); return; }
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery((void*)(uintptr_t)data, &mi, sizeof mi) == 0 || !(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) ||
        (uintptr_t)mi.BaseAddress + mi.RegionSize < (uintptr_t)data + (size_t)4096 * 4096 * 4)
    { Note("difficulty: near terrain: the level's CPU material map is not writable memory; its scatter keeps the level's map"); return; }
    memcpy((void*)(uintptr_t)data, g_coarseMat.pixels, (size_t)4096 * 4096 * 4);
    g_coarseMat.scatter = 2;
    Note("difficulty: near terrain: the level's CPU material map (scatter, physics) is the interpolated one");
}

typedef uint64_t (*NearReadFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t NearMatReadAsync(uint64_t renderer, uint64_t src, uint64_t alloc, uint64_t dst, uint64_t done, uint64_t mip, uint64_t g, uint64_t h)
{
    NearReadFn original = (NearReadFn)((uintptr_t)GetModuleHandleW(NULL) + 0x10B200u);
    const uint32_t* pixels = src == 0 ? NULL : src == g_matband.tex ? g_matband.pixels : src == g_coarseMat.tex ? g_coarseMat.pixels : NULL;
    if (pixels != NULL && (uint16_t)mip == 0 && alloc != 0 && dst != 0 && done != 0)
    {
        typedef void** (*AllocFn)(uint64_t self, void** out, uint64_t bytes, uint64_t align);
        uint64_t vt = 0; AllocFn allocate = NULL;
        void* p = NULL;
        if (!SafeRead(alloc, &vt, 8) && vt != 0 && !SafeRead(vt + 0x30, &allocate, 8) && allocate != NULL)
        {
            void** r = allocate(alloc, &p, (uint64_t)4096 * 4096 * 4, 8);
            if (r != NULL) p = *r;
        }
        if (p != NULL)
        {
            memcpy(p, pixels, (size_t)4096 * 4096 * 4);
            *(uint64_t*)(uintptr_t)dst = (uint64_t)(uintptr_t)p;
            *(volatile uint8_t*)(uintptr_t)done = 1;
            if (src == g_matband.tex) g_matband.served = 1; else g_coarseMat.served = 1;
            return 0;
        }
        /* no memory from the world's allocator: the render target's own readback (the physics keep the game's map) */
        src = src == g_matband.tex ? g_near.tex[1] : g_near.config[1];
    }
    return original(renderer, src, alloc, dst, done, mip, g, h);
}

static uint64_t NearMatReadSync(uint64_t renderer, uint64_t src, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h)
{
    NearReadFn original = (NearReadFn)((uintptr_t)GetModuleHandleW(NULL) + 0x10B0D0u);
    if (src == g_matband.tex && g_matband.tex != 0) { src = g_near.tex[1]; g_matband.served = 2; }
    else if (src == g_coarseMat.tex && g_coarseMat.tex != 0) { src = g_near.config[1]; g_coarseMat.served = 2; }
    return original(renderer, src, c, d, e, f, g, h);
}

/* The two readback calls of the spawn point at the stubs (on) or back at the game's functions (off). 1 when done. */
static int NearMatRedirect(int on)
{
    static const uint32_t sites[2] = {NEAR_MAT_ASYNC_SITE, NEAR_MAT_SYNC_SITE};
    static const uint32_t targets[2] = {0x10B200u, 0x10B0D0u};
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    if (g_matband.stub == NULL)
    {
        if (!on) return 0;
        if (!ApiCodeMatches(exe + sites[0], "E8 CF 00 F6 FF") || !ApiCodeMatches(exe + sites[1], "E8 74 FF F5 FF"))
        { Note("difficulty: near terrain: the spawn's material readback is not the reviewed code; the tile keeps its own map"); return 0; }
        uint8_t* stub = AllocateNearGame(exe, 0x1000);
        if (stub == NULL) { Note("difficulty: near terrain: no room near the exe for the readback stubs; the tile keeps its own map"); return 0; }
        for (int k = 0; k < 2; k++)
        {
            intptr_t reach = (intptr_t)(stub + 16 * k) - (intptr_t)(exe + sites[k] + 5);
            if (reach != (intptr_t)(int32_t)reach) { Note("difficulty: near terrain: the readback stubs are out of reach; the tile keeps its own map"); return 0; }
            uint64_t to = k == 0 ? (uint64_t)(uintptr_t)NearMatReadAsync : (uint64_t)(uintptr_t)NearMatReadSync;
            static const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0}; /* jmp [rip+0] */
            memcpy(stub + 16 * k, jmp, 6); memcpy(stub + 16 * k + 6, &to, 8);
        }
        DWORD old = 0;
        if (!VirtualProtect(stub, 0x1000, PAGE_EXECUTE_READ, &old)) { Note("difficulty: near terrain: the readback stubs are not executable; the tile keeps its own map"); return 0; }
        g_matband.stub = stub;
    }
    uintptr_t first = exe + sites[0], last = exe + sites[1] + 5;
    DWORD old = 0;
    if (!VirtualProtect((void*)first, last - first, PAGE_EXECUTE_READWRITE, &old)) return 0;
    for (int k = 0; k < 2; k++)
    {
        uintptr_t to = on ? (uintptr_t)(g_matband.stub + 16 * k) : exe + targets[k];
        *(volatile int32_t*)(exe + sites[k] + 1) = (int32_t)((intptr_t)to - (intptr_t)(exe + sites[k] + 5));
    }
    VirtualProtect((void*)first, last - first, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)first, last - first);
    return 1;
}

static int NearConformStep(uint64_t records, uint32_t end, uint64_t context, uint64_t g, uint64_t h)
{
    NearReadbackFn readback = (NearReadbackFn)NearApi(0x10, 0x1A8);
    NearReadyFn ready = (NearReadyFn)NearApi(0x10, 0x1D0);
    NearDescFn desc = (NearDescFn)NearApi(0x10, 0x1D8);
    if (readback == NULL || ready == NULL || desc == NULL) return 0;
    if (g_conform.phase == 0) /* the coarse terrain's heights */
    {
        g_conform.id = readback(g_oneMip.level == g_world.levels && g_oneMip.tex != 0 ? g_oneMip.tex : g_near.config[0]); g_conform.polls = 0; g_conform.phase = 1;
        return 1;
    }
    if (g_conform.phase == 9) /* the plan's thread */
    {
        if (!g_conform.planDone) return 1; /* bounded work on the loader's own buffers: it finishes */
        Note("difficulty: near terrain: conform: planned in %.0f ms (on its own thread, %.1f s to the game's frame); the change to draw is mean |d| %.2f m, max %.2f m; %u of %u blocks as two triangles",
            g_conform.ms, (double)(GetTickCount64() - g_conform.planSince) / 1000.0, g_conformPlanMean, g_conformPlanWorst, g_conform.blocksCoarse, (unsigned)(NEAR_BLOCKS * NEAR_BLOCKS));
        if (!g_conform.planOk) return 0;
        g_conform.next = 0; g_conform.waits = 0; g_conform.phase = 2;
        return 1;
    }
    if (g_conform.phase == 1 || g_conform.phase == 5)
    {
        if (!ready(g_conform.id)) { if (++g_conform.polls < 2000) return 1; Note("difficulty: near terrain: conform: a readback did not come"); return 0; }
        uint8_t out[32] = {0}, d[24];
        const uint8_t* p = desc(out, g_conform.id);
        memcpy(d, p ? p : out, 24);
        uint32_t w, hgt, format; uint64_t data;
        memcpy(&w, d, 4); memcpy(&hgt, d + 4, 4); memcpy(&format, d + 8, 4); memcpy(&data, d + 16, 8);
        if (data == 0 || w != 4096 || hgt != 4096 || NearTexelBytes(format) != 2)
        { Note("difficulty: near terrain: conform: a readback is %u x %u format %u; the tile stays as rendered", w, hgt, format); return 0; }
        if (g_conform.phase == 1)
        {
            if (g_conform.planRunning) return 1; /* a plan of a build that failed is still using the buffers */
            if (g_conform.coarse == NULL) g_conform.coarse = (uint16_t*)VirtualAlloc(NULL, (size_t)4096 * 4096 * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (g_conform.coarse == NULL || SafeRead(data, g_conform.coarse, (size_t)4096 * 4096 * 2)) return 0;
            NearRingRestore("the conform's readback of the level's heightmap", NearFetchU16, NearStoreU16, g_conform.coarse);
            g_conform.planDone = 0; g_conform.planOk = 0; g_conform.planSince = GetTickCount64();
            InterlockedExchange(&g_conform.planRunning, 1);
            HANDLE th = CreateThread(NULL, 0, NearConformPlanThread, NULL, 0, NULL);
            if (th == NULL) NearConformPlanThread(NULL); /* no thread: here, as before */
            else CloseHandle(th);
            g_conform.phase = 9;
            return 1;
        }
        /* phase 5: the result; compare with the target at the grid's vertices (exact there) */
        const uint16_t* u = (const uint16_t*)(uintptr_t)data;
        static uint16_t* got;
        if (got == NULL) got = (uint16_t*)VirtualAlloc(NULL, (size_t)4096 * 4096 * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (got == NULL || SafeRead((uintptr_t)u, got, (size_t)4096 * 4096 * 2)) return 0;
        double miss = 0, worst = 0, edge = 0; int n = 0, ne = 0; float edgeMax = 0;
        for (int j = 1; j < NEAR_GRID - 1; j += 3)
            for (int i = 1; i < NEAR_GRID - 1; i += 3)
            {
                size_t at = (size_t)(4 * j) * 4096 + (size_t)(4 * i);
                float want = g_near.heights[at] * 1000.0f - 500.0f + g_conform.delta[(size_t)j * NEAR_GRID + i];
                float have = got[at] / 65535.0f * 1000.0f - 500.0f;
                miss += fabs(have - want); n++; if (fabs(have - want) > worst) worst = fabs(have - want);
            }
        const float x0 = g_near.centre[0] - NEAR_SIDE / 2 + NEAR_TEXEL / 2, y0 = g_near.centre[1] + NEAR_SIDE / 2 - NEAR_TEXEL / 2;
        /* along the hidden square's edge (where the coarse terrain's free edge lies): the tile, bilinear, against the
         * coarse surface, every texel */
        for (int k = 0; k < 4 * 3641; k++)
        {
            int side = k / 3641; float t = -NEAR_HIDE / 2 + (float)(k % 3641) * NEAR_TEXEL;
            float wx = side == 0 ? g_near.centre[0] - NEAR_HIDE / 2 : side == 1 ? g_near.centre[0] + NEAR_HIDE / 2 : g_near.centre[0] + t;
            float wy = side == 2 ? g_near.centre[1] - NEAR_HIDE / 2 : side == 3 ? g_near.centre[1] + NEAR_HIDE / 2 : g_near.centre[1] + t;
            float fx = (wx - x0) / NEAR_TEXEL, fy = (y0 - wy) / NEAR_TEXEL;
            int c = (int)fx, r = (int)fy; float ax = fx - c, ay = fy - r;
            if (c < 0 || r < 0 || c > 4094 || r > 4094) continue;
            const uint16_t* g0 = got + (size_t)r * 4096 + c, *g1 = g0 + 4096;
            float tile = ((g0[0] * (1 - ax) + g0[1] * ax) * (1 - ay) + (g1[0] * (1 - ax) + g1[1] * ax) * ay) / 65535.0f * 1000.0f - 500.0f;
            float e = fabsf(tile - NearCoarseAt(wx, wy));
            edge += e; ne++; if (e > edgeMax) edgeMax = e;
        }
        for (size_t i = 0; i < (size_t)4096 * 4096; i++) g_near.heights[i] = got[i] / 65535.0f;
        Note("difficulty: near terrain: conform: the tile now follows the coarse terrain: at the grid's vertices |result - target| mean %.3f m, max %.3f m; "
            "at the hidden square's edge |tile - coarse| mean %.3f m, max %.3f m", n ? miss / n : 0.0, worst, ne ? edge / ne : 0.0, (double)edgeMax);
        char t[4] = "";
        int dump = GetEnvironmentVariableA("HELLFORGE_XL_NEAR_DUMP", t, sizeof t) > 0 && t[0] == '1';
        if (!dump && !NearMatBandOn() && !NearCoarseMatOn()) return 0;
        g_conform.id = readback(g_near.tex[1]); g_conform.polls = 0; g_conform.phase = 6; /* both material maps: the band (NearMatBuild), the dumps */
        return 1;
    }
    if (g_conform.phase == 6 || g_conform.phase == 7) /* the tile's and the level's material maps: the band, and the dumps (HELLFORGE_XL_NEAR_DUMP) */
    {
        if (!ready(g_conform.id)) return ++g_conform.polls < 2000 ? 1 : 0;
        uint8_t out[32] = {0}, d[24];
        const uint8_t* p = desc(out, g_conform.id);
        memcpy(d, p ? p : out, 24);
        uint32_t w, hgt, format; uint64_t data;
        memcpy(&w, d, 4); memcpy(&hgt, d + 4, 4); memcpy(&format, d + 8, 4); memcpy(&data, d + 16, 8);
        uint32_t bytes = NearTexelBytes(format);
        char t[4] = "";
        int dump = GetEnvironmentVariableA("HELLFORGE_XL_NEAR_DUMP", t, sizeof t) > 0 && t[0] == '1';
        const size_t mapBytes = (size_t)4096 * 4096 * 4;
        uint8_t* copy = NULL;
        if (data != 0 && w == 4096 && hgt == 4096 && bytes == 4)
        {
            copy = (uint8_t*)VirtualAlloc(NULL, mapBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (copy != NULL && SafeRead(data, copy, mapBytes)) { VirtualFree(copy, 0, MEM_RELEASE); copy = NULL; }
        }
        else Note("difficulty: near terrain: %s material map read back as %u x %u format %u, not 4096 x 4096 x 32 bits", g_conform.phase == 6 ? "the tile's" : "the level's",
            w, hgt, format);
        if (dump && copy != NULL && !g_moveBuilding) NearWriteMap(g_conform.phase == 6 ? L"near-tile-materials.bin" : L"near-coarse-materials.bin", copy, mapBytes);
        if (g_conform.phase == 6)
        {
            NearMatDrop();
            g_matband.pixels = (uint32_t*)copy; /* the tile's own map; the band goes in once the level's is read */
            if (g_moveBuilding)
            {
                /* a tile built in mission: the level's map as read at load (the texture under its name is the coarse
                 * terrain's interpolated one now, and the merge's own was released); no coarse rebuild */
                if (g_matband.pixels != NULL && g_levelMat.pixels != NULL && g_levelMat.level == g_world.levels && NearMatBandOn())
                    NearMatBuild(g_levelMat.pixels);
                if (g_matband.tex == 0) NearMatDrop();
                return 0;
            }
            g_conform.id = readback(g_near.config[1]); g_conform.polls = 0; g_conform.phase = 7;
            return 1;
        }
        if (copy != NULL)
        {
            NearCoarseMatBuild((const uint32_t*)copy);
            if (dump && g_coarseMat.tex != 0) NearWriteMap(L"near-coarse-materials-interpolated.bin", (const uint8_t*)g_coarseMat.pixels, mapBytes);
        }
        if (g_matband.pixels != NULL && copy != NULL && NearMatBandOn())
        {
            NearMatBuild((const uint32_t*)copy);
            if (dump && g_matband.tex != 0) NearWriteMap(L"near-tile-materials-band.bin", (const uint8_t*)g_matband.pixels, mapBytes);
        }
        if (copy != NULL)
        {
            /* kept for the tiles built in mission (their band): the level's own map, before the coarse terrain's */
            if (g_levelMat.pixels != NULL) VirtualFree(g_levelMat.pixels, 0, MEM_RELEASE);
            g_levelMat.pixels = (uint32_t*)copy; g_levelMat.level = g_world.levels;
        }
        if (g_matband.tex == 0) NearMatDrop();
        return 0;
    }
    if (g_conform.phase == 2) /* the triangles, a batch per empty queue */
    {
        if (NearQueued(context) != 0) { if (++g_conform.waits < 1200) return 1; Note("difficulty: near terrain: conform: the queue did not empty"); return 0; }
        g_conform.waits = 0;
        if (g_conform.next >= (uint32_t)NEAR_BLOCKS * NEAR_BLOCKS)
        {
            int s64 = -1, s41 = -1, s42 = -1;
            for (int s = 0; s < g_near.steps; s++)
            { if (g_near.stepOp[s] == 0x64) s64 = s; if (g_near.stepOp[s] == 0x41) s41 = s; if (g_near.stepOp[s] == 0x42) s42 = s; }
            if (s64 < 0) return 0;
            /* the materials again (0x41) from the conformed heights before finalize (run v8: the tile's material map against the
             * level's at the edge, mean |d| per channel 37/31/28/4.6 -> 24/24/12/1.5); off with HELLFORGE_XL_NEAR_REMAT=0.
             * The heightmap properties first (0x42: terrain_generator_properties reads generated_heightmap_f, which the
             * conform's triangles wrote; slopes, valleys and peaks): the materials follow the final heights, so the edge
             * band, blended to the coarse terrain, gets the coarse terrain's materials too (run v16: with the replay's
             * properties the tile was rocky to its edge, a straight line against the coarse terrain's sand). */
            char t[4] = "";
            int remat = !(GetEnvironmentVariableA("HELLFORGE_XL_NEAR_REMAT", t, sizeof t) > 0 && t[0] == '0') && s41 >= 0;
            g_conform.seqN = 0;
            if (remat && s42 >= 0) g_conform.seq[g_conform.seqN++] = s42;
            if (remat) g_conform.seq[g_conform.seqN++] = s41;
            g_conform.seq[g_conform.seqN++] = s64;
            g_conform.seqI = 0;
            g_near.step = g_conform.seq[0]; g_near.calls = 0; g_conform.phase = 3;
            if (remat) Note("difficulty: near terrain: conform: the materials are drawn again from the conformed heights");
            return 1;
        }
        if (!NearConformDraw(context)) { Note("difficulty: near terrain: conform: the triangles were not drawn"); return 0; }
        return 1;
    }
    if (g_conform.phase == 3) /* finalize again */
    {
        int r = NearRunStep(records, end, context, g, h);
        if (r < 0) { Note("difficulty: near terrain: conform: op 0x%02X failed", g_near.stepOp[g_near.step]); return 0; }
        if (r > 0)
        {
            if (++g_conform.seqI < g_conform.seqN) { g_near.step = g_conform.seq[g_conform.seqI]; g_near.calls = 0; return 1; }
            g_conform.phase = 4; g_conform.waits = 0;
        }
        return 1;
    }
    if (NearQueued(context) != 0 || ++g_conform.waits < 4) return g_conform.waits < 600 ? 1 : 0;
    g_conform.id = readback(g_near.tex[0]); g_conform.polls = 0; g_conform.phase = 5;
    return 1;
}

/* ---- Step dumps (HELLFORGE_XL_NEAR_STEPDUMP=1, research): the tile's heights after each replayed op that draws heights ----
 * Every height layer of terrain_generator_heightmap (terrain_height ADD, terrain_cap_height MAX) and the later height
 * passes render into generated_heightmap_f; 0x64 (terrain_generator_convert_output: height_convert FROM_32F TO_16UNORM)
 * turns it into the tile's u16 texture, which the replay already reads back. After the level's 0x3C record, after the
 * last vanilla-density tile and after 0x40, 0x3E, 0x3F, 0x3D, 0x0D and 0x63, the queue is drained, 0x64 runs alone, and
 * the readback is written as near-step-NN-XX.u16 (step, op; u16 as the texture). The final 0x64 converts again, so the
 * tile is as without the dumps. 0x64 also rewrites generated_heightmap_source and the slope maps, as the final one does. */
static struct { int phase, back, s64; uint32_t id, polls, waits; } g_stepDump;

static int NearStepDumpOn(void)
{
    static int on = -1;
    if (on < 0) { char t[4] = ""; on = GetEnvironmentVariableA("HELLFORGE_XL_NEAR_STEPDUMP", t, sizeof t) > 0 && t[0] == '1'; }
    return on;
}

static int NearStepDumpAfter(int s)
{
    if (!NearStepDumpOn()) return 0;
    if (g_near.stepKind[s] == NEAR_TILE) return s + 1 >= g_near.steps || g_near.stepKind[s + 1] != NEAR_TILE;
    switch (g_near.stepOp[s]) { case 0x3C: case 0x40: case 0x3E: case 0x3F: case 0x3D: case 0x0D: case 0x63: return 1; }
    return 0;
}

/* 1 while dumping; 0 when done (g_near.step is then the step after the dumped one), -1 on failure. */
static int NearStepDump(uint64_t records, uint32_t end, uint64_t context, uint64_t g, uint64_t h)
{
    if (g_stepDump.phase == 1 || g_stepDump.phase == 3) /* drained, then a few frames for the consumer's last jobs */
    {
        if (NearQueued(context) != 0 || ++g_stepDump.waits < 4) return g_stepDump.waits < 600 ? 1 : -1;
        g_stepDump.waits = 0;
        if (g_stepDump.phase == 1) { g_near.step = g_stepDump.s64; g_near.calls = 0; g_stepDump.phase = 2; return 1; }
        NearReadbackFn readback = (NearReadbackFn)NearApi(0x10, 0x1A8);
        if (readback == NULL) return -1;
        g_stepDump.id = readback(g_near.tex[0]); g_stepDump.polls = 0; g_stepDump.phase = 4;
        return 1;
    }
    if (g_stepDump.phase == 2)
    {
        int r = NearRunStep(records, end, context, g, h);
        if (r < 0) return -1;
        if (r > 0) { g_stepDump.phase = 3; g_stepDump.waits = 0; }
        return 1;
    }
    NearReadyFn ready = (NearReadyFn)NearApi(0x10, 0x1D0);
    NearDescFn desc = (NearDescFn)NearApi(0x10, 0x1D8);
    if (ready == NULL || desc == NULL) return -1;
    if (!ready(g_stepDump.id)) return ++g_stepDump.polls < 2000 ? 1 : -1;
    uint8_t out[32] = {0}, d[24];
    const uint8_t* p = desc(out, g_stepDump.id);
    memcpy(d, p ? p : out, 24);
    uint32_t w, hgt, format; uint64_t data;
    memcpy(&w, d, 4); memcpy(&hgt, d + 4, 4); memcpy(&format, d + 8, 4); memcpy(&data, d + 16, 8);
    int s = g_stepDump.back - 1;
    if (data != 0 && w == 4096 && hgt == 4096 && NearTexelBytes(format) == 2)
    {
        static uint16_t* copy;
        if (copy == NULL) copy = (uint16_t*)VirtualAlloc(NULL, (size_t)4096 * 4096 * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        wchar_t name[64];
        if (g_moveBuilding) /* a tile built in mission: named by its centre */
            swprintf(name, 64, L"near-step-%d-%d-%02d-%02X%ls.u16", (int)lroundf(g_near.centre[0]), (int)lroundf(g_near.centre[1]), s, (unsigned)g_near.stepOp[s],
                g_near.stepKind[s] == NEAR_TILE ? L"t" : L"");
        else swprintf(name, 64, L"near-step-%02d-%02X%ls.u16", s, (unsigned)g_near.stepOp[s], g_near.stepKind[s] == NEAR_TILE ? L"t" : L"");
        HANDLE f = OpenLog(name);
        if (copy != NULL && f != INVALID_HANDLE_VALUE && !SafeRead(data, copy, (size_t)4096 * 4096 * 2))
        {
            DWORD wrote = 0; WriteFile(f, copy, 4096u * 4096u * 2u, &wrote, NULL);
            double sum = 0; uint16_t lo = 65535, hi = 0;
            for (size_t i = 0; i < (size_t)4096 * 4096; i += 64) { sum += copy[i]; if (copy[i] < lo) lo = copy[i]; if (copy[i] > hi) hi = copy[i]; }
            Note("difficulty: near terrain: step dump after step %d (op 0x%02X%s): heights %.1f..%.1f m, mean %.1f", s, g_near.stepOp[s],
                g_near.stepKind[s] == NEAR_TILE ? ", the last vanilla-density tile" : "", lo / 65535.0 * 1000 - 500, hi / 65535.0 * 1000 - 500,
                sum / ((double)4096 * 4096 / 64) / 65535.0 * 1000 - 500);
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    else Note("difficulty: near terrain: step dump after step %d: the readback is %u x %u format %u", s, w, hgt, format);
    g_near.step = g_stepDump.back; g_near.calls = 0; g_stepDump.phase = 0;
    return 0;
}

/* ---- Stamps in rings around the drop point (static analysis: knowledge base, xl-world-streaming, 2026-11-03) ----
 * PlaceStampWithWeightRule (op 0x2B, 0xF34CC0) picks a fixed number of route-cell groups per rule ([rule+0x20] against a
 * permutation of the items, 0xF327D9), whatever the map's size: a 16 km level's 290 route cells (~650 m apart) get about
 * one vanilla map's stamps over 200+ km^2, so the rocks, cliffs and mesas that make vanilla's relief are 20-75x sparser.
 * Here, once the level's own 0x2B has run (record 91, before 0x5F hashes the locations and 0x31/0x2F spawn them), the op
 * runs again on vanilla-density composite tiles (TerrainBuild, 16 m Voronoi, ~40 m route cells) around the drop point,
 * and the stamps it places are filtered by distance (rings: all near the drop, larger ones only further out) and appended
 * to the level's locations, so 0x31 (units), 0x2F (stamp terrains), 0x63 (heights), the near replay and the streamer take
 * them as their own.
 * What the tile run needs beyond TerrainBuild (proven unless noted):
 *  - 0x4E GenerateOutsideMapDistance (G+123FE8, read at 0xF33893) and 0x26 GenerateFbr (G+106018, stamp angles,
 *    0xF33E75) are CPU passes over the Voronoi vertices: rerun on the tile with its vertices moved to level metres and
 *    the level's settings, 0x26 from its record as it was before it ran (its permutation comes from the record's RNG,
 *    +0x1990, which it advances), so both match the level's fields at those points.
 *  - the per-vertex clearing byte (G+11A0020; 0xF3487E, 0xF34164, 0xF319C1) is 0xB3's: every vertex within 1.01 r of a
 *    level clearing point (G+11A16FC, 16 bytes: x, y, r, flags; count G+11A7244) is marked, the rest cleared.
 *  - the run's own stamp index ([ctx+0x20]) over 0..896: the level's index holds level objects in level metres.
 *  - the world gates use settings+0x2C/+0x50/+0x54/+0x5C (0xF32816, 0xF346E3, 0xF347D9): widened for the tile run, the
 *    level's applied to the result here (per location, with its rule flags loc+0xD4 bit 0).
 *  - stamps of a type the level did not load are dropped: packages are queued per type of the level's list (0xA5,
 *    0xC194FA: G+18008, count G+11A7080; 0xF41F80), and a location's type is loc+0xD8 (the cell entry's +0xC, 0xF32813).
 *  - the level's index then tests each kept stamp against the level's locations (flags bit 0 clear: [P+0x2050] + id x
 *    0xE0, box +0xB0) and roads (bit 0 set: the segment from point id - 1 to id of [P+0x2060] = G+9568E4, float x, y
 *    each, half the width at +0x30 of the route [P+0x2058] = G+937348 + (flags >> 2) x 0x48; 0xF30D40, 0xFCC180) before
 *    it is inserted (0xC04EF0, slot = the location's number), as the placement loop does.
 * Locations: 0xE0 at G+11A7280 (xy +0, box +0xB0..+0xBC: moved to level metres), extras 0x40 at G+DIF_LOCATIONS_B_NEW (read by no op
 * after record 91; +0x10 = the record, +0x38 the vertex list's length: cleared), count G+11A7108 and G+11A710C. */
#define STAMP_TILE_CELLS_VANILLA 430.0f /* a vanilla map's route-cell items: 434 (planet 70) and 425 (planet 259), measured */
#define STAMP_LOCATION_LIMIT (DIF_GEN_LOCATIONS - 192u) /* of the generation's DIF_GEN_LOCATIONS */
typedef struct { uint8_t loc[0xE0]; uint8_t extra[0x40]; float side, prio; uint32_t ring, hash; } StampCandidate;
static struct
{
    LONG level;                 /* g_world.levels the centre and the records are for */
    float centre[2];            /* world metres, on the near tile's 256 m grid */
    LONG rec26Level, rec4ELevel;
    uint8_t rec26[0x19B0], rec4E[0x19B0];
    StampCandidate* cand;
    uint32_t count, capacity, reserved;
    LONG ringLevel;             /* g_world.levels the rings below are for */
    uint32_t first, end;        /* the added locations' indices: first .. end - 1 */
    uint8_t ring[STAMP_LOCATION_LIMIT];
} g_stamps = {.level = -1, .rec26Level = -1, .rec4ELevel = -1, .ringLevel = -1};

/* The content streamer (difficulty_stream.c) asks this for every location it defers at generation: 1 when the location
 * is a stamp added here in ring 1 or 2. Those rings hold only the large formations, which make the skyline from afar, so
 * the streamer spawns them after generation wherever the players are, under its physics budget, and keeps them (the rest
 * spawn within 500 m of a player and go past 800 m). A layer cannot be spawned without its non-visual members (0xDBEC0
 * spawns every member kind of a selected layer), so they bring their whole default content; objective layers only
 * spawn where the plan put an objective, which added stamps never have. Spawning all of them with the level (run s70i:
 * 3069) filled the physics body store. HELLFORGE_XL_FAR_STAMPS=0 streams them like the rest. */
static int StampsKeepFar(uint32_t i)
{
    static int on = -1;
    if (on < 0) { char v[8]; DWORD k = GetEnvironmentVariableA("HELLFORGE_XL_FAR_STAMPS", v, sizeof v); on = !(k > 0 && k < sizeof v && v[0] == '0'); }
    return on && g_stamps.ringLevel == g_world.levels && i >= g_stamps.first && i < g_stamps.end && g_stamps.ring[i] >= 1;
}

static int StampsOn(void)
{
    static int on = -1;
    if (on < 0) { char t[8] = ""; on = !(GetEnvironmentVariableA("HELLFORGE_XL_STAMPS", t, sizeof t) > 0 && t[0] == '0'); }
    return on;
}

/* Rings (Chebyshev metres from the centre) and what each keeps: HELLFORGE_XL_STAMP_RINGS="r0,r1,r2,side1,side2,share1". */
static void StampRings(float* r)
{
    static float v[6] = {1088.0f, 2048.0f, 4096.0f, 16.0f, 32.0f, 0.25f};
    static int read;
    if (!read)
    {
        read = 1;
        char t[96] = "";
        if (GetEnvironmentVariableA("HELLFORGE_XL_STAMP_RINGS", t, sizeof t) > 0)
        {
            float w[6];
            if (sscanf(t, "%f,%f,%f,%f,%f,%f", &w[0], &w[1], &w[2], &w[3], &w[4], &w[5]) == 6 && w[0] > 0 && w[1] >= w[0] && w[2] >= w[1])
                memcpy(v, w, sizeof v);
        }
    }
    memcpy(r, v, sizeof v);
}

/* Before the level runs op 0x26 or 0x4E (difficulty_world.c's WorldDispatch): their records as they are then. */
static void TerrainStampsRecord(uint64_t record, uint32_t op)
{
    if (op == 0x26 && g_stamps.rec26Level != g_world.levels && !SafeRead(record, g_stamps.rec26, sizeof g_stamps.rec26)) g_stamps.rec26Level = g_world.levels;
    if (op == 0x4E && g_stamps.rec4ELevel != g_world.levels && !SafeRead(record, g_stamps.rec4E, sizeof g_stamps.rec4E)) g_stamps.rec4ELevel = g_world.levels;
}

static int StampWritable(uintptr_t at, size_t bytes)
{
    MEMORY_BASIC_INFORMATION m;
    if (VirtualQuery((void*)at, &m, sizeof m) != sizeof m || m.State != MEM_COMMIT) return 0;
    if ((uintptr_t)m.BaseAddress + m.RegionSize < at + bytes) return 0;
    return (m.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0;
}

static void StampShiftVertices(uint8_t* T, uint32_t vertices, float dx, float dy)
{
    for (uint32_t v = 0; v < vertices; v++)
    {
        float xy[2];
        memcpy(xy, T + 0x9F310 + (size_t)v * 0x48, 8);
        xy[0] += dx; xy[1] += dy;
        memcpy(T + 0x9F310 + (size_t)v * 0x48, xy, 8);
    }
}

static int StampRunRecord(const uint8_t* saved, uintptr_t gen, uint8_t* T, uint64_t seed, uint64_t context, uint64_t g, uint64_t h, int limit)
{
    static uint8_t oneGenerator[0x19B0];
    TileRunner* runner = TileRunnerSelf();
    uint8_t* one = runner != NULL ? runner->one : oneGenerator;
    memcpy(one, saved, 0x19B0);
    WorldTileRelocate(one, gen, (uintptr_t)T);
    if (seed) { uint64_t r; memcpy(&r, one + 0x1990, 8); r ^= seed; memcpy(one + 0x1990, &r, 8); }
    volatile uint32_t index = 0;
    volatile int calls = 0;
    uint8_t phase = 0;
    LONG fault = WorldTileGuarded(one, context, &index, &phase, g, h, limit, &calls);
    if (!fault && index != 0) WorldArenaReset(context);
    return fault ? -1 : index != 0 ? 1 : 0;
}

/* The tile run's stamp index over 0..896: the level's header (its other tables at +0x2058/+0x2060), its own memory,
 * built as the level init builds the level's (WorldIndex), locations from the tile's table. */
static volatile LONG64 g_emptyIndexQueries; /* difficulty_maps.c IndexQueryClamped */
static uintptr_t StampIndex(TileRunner* r, uintptr_t level, uint8_t* T, float extent)
{
    WorldIndexSize size = (WorldIndexSize)(g_world.game + DIF_INDEX_SIZE);
    uint32_t bytes = size(extent, extent, extent / 64.0f);
    if (r->memory == NULL || r->memoryBytes < bytes)
    {
        if (r->memory != NULL) VirtualFree(r->memory, 0, MEM_RELEASE);
        r->memory = (uint8_t*)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        r->memoryBytes = r->memory ? bytes : 0;
    }
    if (r->header == NULL || r->memory == NULL || SafeRead(level, r->header, 0x2068)) return 0;
    uintptr_t p = (uintptr_t)r->header, memory = (uintptr_t)r->memory, table = (uintptr_t)(T + 0x11A7280);
    memcpy((void*)p, &extent, 4);
    memset((void*)memory, 0, bytes);
    memset((void*)(p + 0x50), 0, 0x2000);
    memcpy((void*)(p + 0x48), &memory, 8);
    ((WorldIndexInit)(g_world.game + DIF_INDEX_INIT))(p + 8, 0.0f, 0.0f, extent, extent, extent / 64.0f, 0, 0, 0, 0, memory);
    /* A tile run's index is narrow: the constructor gives every index the level's 131071 slots (kStampIndex), and every
     * query reads [+0x28] qwords of each row and column it spans (0xC0538F), so the planning's op 0x2B time follows the
     * width (s76b 0x800: 1.12 ms a stamp, s76c 0x400: 0.70). Only the tile's op 0x2B inserts here (a fresh index per
     * tile; at most 3946 records in one tile run over s75y / s76a / s76c / s76d), so rows of 0x100 qwords (16384 slots)
     * and a free list cut at 16383 (entry i holds i + 1, 0xC04D00): the insert (0xC04F08), the query (0xC0538F,
     * 0xC05437) and the remove read the width, stride +0x24 and entries +0x30 from the header; the rows pack closer in
     * memory sized for 0x800. The insert writes through a null when the list runs out (0xF3516C), so the note after
     * each run counts the tile's most records against these slots. */
    {
        uint32_t words = 0x100, slotsPlusOne = 0x4000, end = 0xFFFFFFFFu, have = 0; uint16_t stride = 0; uint64_t entries = 0;
        uintptr_t I = p + 8;
        if (!SafeRead(I + 0x24, &stride, 2) && !SafeRead(I + 0x30, &entries, 8) && !SafeRead(I + 0x28, &have, 4) && entries != 0 && stride == 4 && have > words)
        {
            memcpy((void*)(I + 0x28), &words, 4); memcpy((void*)(I + 0x1C), &slotsPlusOne, 4);
            memcpy((void*)(uintptr_t)(entries + 4ull * (slotsPlusOne - 1)), &end, 4);
        }
    }
    memset((void*)(p + 4), 0, 4);
    memcpy((void*)(p + 0x2050), &table, 8);
    return p;
}

static int StampCompare(const void* a, const void* b)
{
    const StampCandidate* x = (const StampCandidate*)a; const StampCandidate* y = (const StampCandidate*)b;
    if ((x->ring == 0) != (y->ring == 0)) return x->ring == 0 ? -1 : 1;
    if (x->prio != y->prio) return x->prio > y->prio ? -1 : 1;
    return x->hash < y->hash ? -1 : x->hash > y->hash ? 1 : 0;
}

/* Distance from a segment to a box (2D), for the level's roads in its stamp index. */
static float StampSegmentBox(const float* s, const float* lo, const float* hi)
{
    float best = 1e30f;
    for (int k = 0; k <= 16; k++)
    {
        float t = (float)k / 16.0f, x = s[0] + (s[2] - s[0]) * t, y = s[1] + (s[3] - s[1]) * t;
        float dx = x < lo[0] ? lo[0] - x : x > hi[0] ? x - hi[0] : 0.0f, dy = y < lo[1] ? lo[1] - y : y > hi[1] ? y - hi[1] : 0.0f;
        float d = sqrtf(dx * dx + dy * dy);
        if (d < best) best = d;
    }
    return best;
}

/* The plan (the stamps the generation's room does not hold): g_stamps.cand[0 .. g_plan.count), ordered by a grid of
 * STAMP_PLAN_GRID x STAMP_PLAN_GRID buckets over the world (generator metres: bucket (x / cell, y / cell)), bucket b's
 * entries at [start[b], start[b + 1]); slot[i] = the window's location slot holding entry i, 0 when none. */
#define STAMP_PLAN_MAX 2000000u /* reserved; committed as it grows (304 B each) */
#define STAMP_PLAN_GRID 64
static struct { LONG level; uint32_t count; float cell; uint32_t start[STAMP_PLAN_GRID * STAMP_PLAN_GRID + 1]; uint16_t* slot; } g_plan = {.level = -1};
static int StampGrow(void) /* 64k more candidates committed; 0 at the reserve */
{
    if (g_stamps.capacity >= g_stamps.reserved) return 0;
    uint32_t more = g_stamps.reserved - g_stamps.capacity < 65536u ? g_stamps.reserved - g_stamps.capacity : 65536u;
    if (VirtualAlloc(g_stamps.cand + g_stamps.capacity, sizeof(StampCandidate) * (size_t)more, MEM_COMMIT, PAGE_READWRITE) == NULL) return 0;
    g_stamps.capacity += more;
    return 1;
}
static float g_planCell = 256.0f;
static int StampPlanBucket(const StampCandidate* c)
{
    float xy[2]; memcpy(xy, c->loc, 8);
    int i = (int)(xy[0] / g_planCell), j = (int)(xy[1] / g_planCell);
    i = i < 0 ? 0 : i >= STAMP_PLAN_GRID ? STAMP_PLAN_GRID - 1 : i; j = j < 0 ? 0 : j >= STAMP_PLAN_GRID ? STAMP_PLAN_GRID - 1 : j;
    return j * STAMP_PLAN_GRID + i;
}
static int StampPlanCompare(const void* a, const void* b)
{
    int x = StampPlanBucket((const StampCandidate*)a), y = StampPlanBucket((const StampCandidate*)b);
    if (x != y) return x < y ? -1 : 1;
    return StampCompare(a, b); /* in a bucket: the larger first */
}
/* plan-<level>.tsv (evidence: the whole map's stamps): index, op 0x2B, world metres x / y, type (+0xD8), flags (+0xD4),
 * the footprint's side (m). */
static void StampPlanWrite(void)
{
    wchar_t name[48];
    swprintf(name, 48, L"plan-%ld.tsv", (long)g_world.levels);
    HANDLE file = OpenLog(name);
    if (file == INVALID_HANDLE_VALUE) return;
    static char buffer[1 << 16];
    size_t used = 0;
    DWORD wrote;
    const float half = 1024.0f * g_world.current;
    for (uint32_t i = 0; i < g_plan.count; i++)
    {
        const StampCandidate* c = &g_stamps.cand[i];
        float xy[2]; uint32_t type; uint16_t flags;
        memcpy(xy, c->loc, 8); memcpy(&type, c->loc + 0xD8, 4); memcpy(&flags, c->loc + 0xD4, 2);
        if (used > sizeof buffer - 128) { WriteFile(file, buffer, (DWORD)used, &wrote, NULL); used = 0; }
        used += (size_t)snprintf(buffer + used, sizeof buffer - used, "%u\t0x2B\t%.1f\t%.1f\t%u\t0x%X\t%.0f\n", i, (double)(xy[0] - half), (double)(xy[1] - half), type, flags,
            (double)c->side);
    }
    if (used) WriteFile(file, buffer, (DWORD)used, &wrote, NULL);
    CloseHandle(file);
}
static void StampPlanBuild(uint32_t count, float world)
{
    g_plan.count = 0; g_plan.level = -1;
    g_planCell = world / (float)STAMP_PLAN_GRID; g_plan.cell = g_planCell;
    if (count == 0) { memset(g_plan.start, 0, sizeof g_plan.start); g_plan.level = g_world.levels; return; }
    qsort(g_stamps.cand, count, sizeof *g_stamps.cand, StampPlanCompare);
    if (g_plan.slot != NULL) { VirtualFree(g_plan.slot, 0, MEM_RELEASE); g_plan.slot = NULL; }
    g_plan.slot = (uint16_t*)VirtualAlloc(NULL, (size_t)count * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_plan.slot == NULL) { Note("difficulty: stamps: no memory for the plan's slots; no plan"); return; }
    uint32_t b = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        int k = StampPlanBucket(&g_stamps.cand[i]);
        while ((int)b <= k) g_plan.start[b++] = i;
    }
    while (b <= STAMP_PLAN_GRID * STAMP_PLAN_GRID) g_plan.start[b++] = count;
    g_plan.count = count;
    g_plan.level = g_world.levels;
    StampPlanWrite();
}

typedef uint32_t (*StampKeyFn)(uintptr_t index, float x, float y);
typedef uint32_t (*StampQueryFn)(uintptr_t index, uint32_t lo, uint32_t hi, uint64_t* out, uint32_t max);
typedef uint32_t* (*StampInsertFn)(uintptr_t index, float x0, float y0, float x1, float y1, void* out);

/* ---- The stamp planning's tiles in parallel (load time) ------------------------------------------------------------
 * Every tile's run is independent of the others (~/xl-work design notes; P): each works on its own copy of the
 * generator block with its records moved onto it, and op 0x2B's items each seed their own LCG and test only their own
 * records and the run's index. So the generator thread builds tiles (TerrainBuild, 0x4E/0x26, the clearing bytes) into
 * slots, each slot its own block, and runners take built slots and run op 0x2B on them, each with its own context
 * (TileRunner: arena, settings, stamp index; the level's context and settings as the section took them otherwise) and
 * op 0x2B's eight jobs inline on the runner's thread (TileJobsInline). The generator commits finished slots in the
 * order of order[] (the candidates' order is the serial code's, so the qsort and everything after it are unchanged)
 * and runs a built slot itself when it has nothing to build or no free slot.
 * With one runner (HELLFORGE_XL_TILE_THREADS=1, or the runners' parts not installed) the generator does every step
 * itself, op 0x2B's jobs submitted as the game does. */
#define STAMP_SLOTS_MAX (TILE_RUNNERS_MAX + 4)
#define STAMP_TILE_CANDIDATES 8192u
typedef struct
{
    volatile LONG state;
    int k, t;                   /* order[] index, tile */
    uint8_t* T;                 /* the slot's generator block */
    float ox, oy, scale;
    uint32_t items;
    int built, fixed, indexed, ran;
    uint32_t made, count, why[8], sides[4][8];
    StampCandidate* cand;       /* STAMP_TILE_CANDIDATES */
    double t2B, tCollect, tBuild, tFix;
    uint64_t emptyQueries;
} StampSlot;
static struct
{
    uint64_t records, context, g, h, rec91, table91;
    uint32_t end;
    uintptr_t gen, P;
    uint32_t types[256], typeCount, clearingCount;
    float rings[6], world, half, s54, s5C, cgx, cgy;
    uint8_t ctx[0xE8], settings2B[0x120], levelS[0x120], buildS[0x120];
    uint8_t rec91Copy[0x19B0];
    StampSlot slots[STAMP_SLOTS_MAX];
    int slotCount, inlineJobs;
    int stageB;                 /* runners build the tiles too (a runner takes a free slot, then the next tile) */
    int tiles, order[1024];
    volatile LONG nextTile, slotOf[1024];
} g_stampPar;

/* A slot's tile: built, 0x4E/0x26 in level metres, its clearing bytes (generator thread: TerrainBuild and the level's
 * context; g_terrain.block is the slot's block meanwhile). */
static void StampTileBuild(TileRunner* r, StampSlot* s)
{
    /* a runner (stage B: its TLS slot is set) builds in its own slot block with a context of its own: the build with the
     * section's settings (the marker flipped), 0x4E/0x26 with the level's; the generator builds in g_terrain.block
     * with the level's context, as the serial code did */
    uint64_t buildContext = g_stampPar.context, fixContext = g_stampPar.context;
    const float core = (float)TERRAIN_TILE_CORE, margin = (float)TERRAIN_TILE_MARGIN, extent = (float)TERRAIN_TILE_EXTENT;
    const int t = s->t;
    s->ox = (float)(t % g_terrain.columns) * core - margin; s->oy = (float)(t / g_terrain.columns) * core - margin;
    s->built = s->fixed = s->indexed = s->ran = 0; s->made = s->count = 0; s->scale = 1.0f; s->items = 0;
    memset(s->why, 0, sizeof s->why); memset(s->sides, 0, sizeof s->sides);
    s->t2B = s->tCollect = s->tBuild = s->tFix = 0; s->emptyQueries = 0;
    double* tBuild = &s->tBuild; double* tFix = &s->tFix;
    LARGE_INTEGER qf, q0, q1; QueryPerformanceFrequency(&qf);
    uint8_t* keep = g_terrain.block;
    uint8_t* T = s->T;
    if (r != NULL)
    {
        r->block = T;
        TileRunnerContext(r, g_stampPar.ctx, g_stampPar.buildS, g_stampPar.P);
        buildContext = fixContext = (uint64_t)(uintptr_t)r->ctx;
    }
    else g_terrain.block = T;
    QueryPerformanceCounter(&q0);
    int ok = TerrainBuild(t, g_stampPar.records, g_stampPar.end, buildContext, g_stampPar.g, g_stampPar.h, g_stampPar.gen, 0);
    QueryPerformanceCounter(&q1); *tBuild += (double)(q1.QuadPart - q0.QuadPart) / (double)qf.QuadPart;
    uint32_t vertices = 0;
    if (ok) { s->built = 1; memcpy(&vertices, T + 0x11A7090, 4); }
    if (ok && vertices != 0 && vertices <= 6000)
    {
        /* 0x4E and 0x26 in level metres, with the level's settings (TerrainBuild put them back) */
        QueryPerformanceCounter(&q0);
        StampShiftVertices(T, vertices, s->ox, s->oy);
        if (r != NULL) memcpy(r->settings, g_stampPar.levelS, sizeof r->settings); /* 0x4E/0x26 with the level's settings */
        s->fixed = (g_stamps.rec4ELevel != g_world.levels || StampRunRecord(g_stamps.rec4E, g_stampPar.gen, T, 0, fixContext, g_stampPar.g, g_stampPar.h, 1000) == 1) &&
                   (g_stamps.rec26Level != g_world.levels || StampRunRecord(g_stamps.rec26, g_stampPar.gen, T, 0, fixContext, g_stampPar.g, g_stampPar.h, 1000) == 1);
        StampShiftVertices(T, vertices, -s->ox, -s->oy);
        QueryPerformanceCounter(&q1); *tFix += (double)(q1.QuadPart - q0.QuadPart) / (double)qf.QuadPart;
        if (s->fixed)
        {
            /* the clearing bytes, from the level's clearing points */
            memset(T + 0x11A0020, 0, vertices);
            for (uint32_t c = 0; c < g_stampPar.clearingCount; c++)
            {
                float q[3];
                if (SafeRead(g_stampPar.gen + 0x11A16FCu + 16ull * c, q, 12) || !(q[2] > 0.0f)) continue;
                float r2 = (1.01f * q[2]) * (1.01f * q[2]), lx = q[0] - s->ox, ly = q[1] - s->oy;
                if (lx < -q[2] || ly < -q[2] || lx > extent + q[2] || ly > extent + q[2]) continue;
                for (uint32_t v = 0; v < vertices; v++)
                {
                    float xy[2]; memcpy(xy, T + 0x9F310 + (size_t)v * 0x48, 8);
                    if ((xy[0] - lx) * (xy[0] - lx) + (xy[1] - ly) * (xy[1] - ly) < r2) T[0x11A0020 + v] = 1;
                }
            }
            memcpy(&s->items, T + 0x11A71C4, 4);
            s->scale = s->items > 0 && (float)s->items < STAMP_TILE_CELLS_VANILLA ? (float)s->items / STAMP_TILE_CELLS_VANILLA : 1.0f;
        }
    }
    if (r != NULL) r->block = NULL; else g_terrain.block = keep;
}

/* The rules' picks scaled to the tile's items in the shared rule tables, for a run whose jobs the game runs (one run at
 * a time); TileJobsInline maps the jobs' permutation instead. */
static uintptr_t g_stampRuleAt[4096]; static uint32_t g_stampRuleOld[4096], g_stampRules;
static void StampRulesScale(const uint8_t* T, uint32_t items, float scale)
{
    g_stampRules = 0;
    if (!(scale < 1.0f)) return;
    for (uint32_t i = 0; i < items && i < 1024; i++)
    {
        int32_t type = -1; uint64_t T8 = 0, list = 0; uint32_t n = 0;
        memcpy(&type, T + 0x92F328 + (size_t)i * 0x20 + 0xC, 4);
        if (type < 0 || type > 4096 || SafeRead(g_stampPar.table91 + 8ull * (uint32_t)type, &T8, 8) || T8 == 0 || SafeRead(T8 + 0x10, &list, 8) || SafeRead(T8 + 0x18, &n, 4) || list == 0 || n > 64) continue;
        for (uint32_t r = 0; r < n && g_stampRules < 4096; r++)
        {
            uintptr_t a = (uintptr_t)list + 0x70ull * r + 0x20;
            int seen = 0;
            for (uint32_t q = 0; q < g_stampRules && !seen; q++) seen = g_stampRuleAt[q] == a;
            uint32_t old = 0;
            if (seen || SafeRead(a, &old, 4) || old == 0 || !StampWritable(a, 4)) continue;
            g_stampRuleAt[g_stampRules] = a; g_stampRuleOld[g_stampRules] = old; g_stampRules++;
            uint32_t now = (uint32_t)floorf((float)old * scale + 0.5f);
            *(volatile uint32_t*)a = now < 1 ? 1 : now;
        }
    }
}
static void StampRulesRestore(void)
{
    for (uint32_t q = 0; q < g_stampRules; q++) *(volatile uint32_t*)g_stampRuleAt[q] = g_stampRuleOld[q];
    g_stampRules = 0;
}

static void StampRunnerLoop(TileRunner* r);

/* Op 0x2B's eight jobs on a runner's own thread. The main body (0xF34CC0) fills the jobs (0x60 each, a pointer to each
 * in the array r14, edi of them; 0xF34EE0..0xF34F6A) and submits them at 0xF34FB5 through [[game+0x33263E8]+0x88](fn
 * 0xF322F0, the array, a name, r9d 2, arg5 4, count, 0): queue 2 + 3 x 4 = 14, which only the two LP workers and the
 * generator thread's help call pop. A runner would leave its jobs there and spin in the wait loop
 * 0xF34FC0..0xF34FF3 (which reads each job's done byte +0x58). The stub at 0xF34FA7 (mov rax, [rip -> 0x33263E8],
 * 7 bytes, rsp 16-aligned with the submit's stack arguments in place) tests the thread's runner (TEB TlsSlots,
 * g_tileRunnerTls): none, the original submit and back to 0xF34FB8; a runner, TileJobsInline(r14, edi) and on to
 * 0xF34FF5 (r15 from the frame, then the serial placement loop; edi, r14 and rbx = 0 are callee-saved), or to
 * 0xF3519E with no jobs, as the original's test at 0xF34FB8 does. */
#define TILE_SUBMIT_SITE 0xF34FA7u
static const char kTileSubmitPin[] = "48 8B 05 3A 14 3F 02 4C 8B 90 88 00 00 00 41 FF D2 85 FF 0F 84 DE 01 00 00";
static const char kTileAfterJobsPin[] = "4C 8B 7C 24 48 8B EF";
static const char kTileNoJobsPin[] = "48 8B 8C 24 A0 00 00 00";
/* op 0x2B's dispatcher case: the label call, then 0xF34CC0(rcx rsi + 8, rdx rsi, r8 r15) and the index step 0xC19E1C */
static const char kTileCase2BPin[] = "48 8D 4E 08 4D 8B C7 48 8B D6 E8 B1 F4 31 00 E9";

/* Each job, in order, on this thread, into the runner's own region (job+0x20, as WorldPlaceJob gives one on a larger
 * map; the merge reads the records through it), the rules' picks scaled to the tile's items through the jobs'
 * permutation: the job picks item x for a rule when perm[x] < [rule+0x20] (0xF327D9..0xF327E9, unsigned; perm =
 * [job+0x30], u32, shared by the jobs, length the item count [[[job+0x10]+0x170]], 0xF323A5/0xF32423/0xF32552). The
 * serial path scaled each rule's count to max(1, floor(old s + 0.5)); with perm'[x] = h(perm[x]) - 1, h(p) = the least
 * o >= 1 whose scaled count exceeds p, perm'[x] < old holds exactly when perm[x] < the scaled count, for every rule
 * (old 0 picks nothing either way). The jobs only read perm (P: its writes are the main body's shuffle, 0xF34E70). */
static void TileJobsInline(uint64_t* jobs, uint32_t n)
{
    TileRunner* r = TileRunnerSelf();
    WorldJobFn job = g_placeJob != NULL ? g_placeJob : (WorldJobFn)(g_world.game + 0xF322F0u);
    if (r == NULL || n == 0) return;
    if (r->scale < 1.0f)
    {
        uint64_t pass = 0, list = 0, perm = 0; uint32_t items = 0;
        memcpy(&pass, (const void*)(uintptr_t)(jobs[0] + 0x10), 8); memcpy(&perm, (const void*)(uintptr_t)(jobs[0] + 0x30), 8);
        if (pass != 0 && !SafeRead(pass + 0x170, &list, 8) && list != 0 && !SafeRead(list, &items, 4) && perm != 0 && items <= 0x100000)
        {
            uint32_t* p = (uint32_t*)(uintptr_t)perm;
            const float scale = r->scale;
            for (uint32_t x = 0; x < items; x++)
            {
                /* the least o >= 1 with max(1, floor(o s + 0.5)) > p: floor(o s + 0.5) >= p + 1 (p >= 1) */
                uint32_t v = p[x], o;
                if (v == 0) o = 1;
                else
                {
                    o = (uint32_t)(((double)v + 0.5) / (double)scale);
                    if (o < 1) o = 1;
                    while (o > 1 && (uint32_t)floorf((float)(o - 1) * scale + 0.5f) >= v + 1) o--;
                    while ((uint32_t)floorf((float)o * scale + 0.5f) < v + 1) o++;
                }
                p[x] = o - 1;
            }
        }
    }
    for (uint32_t j = 0; j < n; j++)
    {
        uint64_t at = jobs[j];
        if (r->regions[j & 7] != NULL) *(volatile uint64_t*)(uintptr_t)(at + 0x20) = (uint64_t)(uintptr_t)r->regions[j & 7];
        job(0, 0, 0, at);
    }
}

static int TileJobsInstall(void)
{
    static int done = -1;
    if (done >= 0) return done;
    done = 0;
    uintptr_t game = g_world.game;
    if (g_tileRunnerTls == TLS_OUT_OF_INDEXES || g_tileRunnerTls >= 64)
    { Note("difficulty: stamps: no low TLS slot for the planning's runners; one runner"); return 0; }
    if (!ApiCodeMatches(game + TILE_SUBMIT_SITE, kTileSubmitPin) || !ApiCodeMatches(game + 0xF34FF5u, kTileAfterJobsPin) || !ApiCodeMatches(game + 0xF3519Eu, kTileNoJobsPin) ||
        !ApiCodeMatches(game + 0xC15800u, kTileCase2BPin))
    { Note("difficulty: stamps: op 0x2B's job submit is not the reviewed one; one runner"); return 0; }
    uint8_t* stub = AllocateNearGame(game, 0x100);
    if (stub == NULL) { Note("difficulty: stamps: no memory near game.dll for the runners' stub; one runner"); return 0; }
    uint8_t c[128]; int n = 0;
    uint32_t teb = 0x1480u + 8u * g_tileRunnerTls;
    uint64_t v;
#define B(...) do { const uint8_t t_[] = {__VA_ARGS__}; memcpy(c + n, t_, sizeof t_); n += (int)sizeof t_; } while (0)
#define Q(x) do { v = (uint64_t)(x); memcpy(c + n, &v, 8); n += 8; } while (0)
    B(0x65, 0x48, 0x8B, 0x04, 0x25); memcpy(c + n, &teb, 4); n += 4;   /* mov rax, gs:[TlsSlots + 8k] */
    B(0x48, 0x85, 0xC0);                                              /* test rax, rax */
    B(0x75, 0x00); int jnz = n - 1;                                   /* jnz runner */
    B(0x48, 0xB8); Q(game + 0x33263E8u);                              /* mov rax, &[game+0x33263E8] */
    B(0x48, 0x8B, 0x00);                                              /* mov rax, [rax] */
    B(0x4C, 0x8B, 0x90, 0x88, 0x00, 0x00, 0x00);                      /* mov r10, [rax+0x88] */
    B(0x41, 0xFF, 0xD2);                                              /* call r10 */
    B(0xFF, 0x25, 0, 0, 0, 0); Q(game + 0xF34FB8u);                   /* jmp game+0xF34FB8 */
    c[jnz] = (uint8_t)(n - (jnz + 1));
    B(0x85, 0xFF);                                                    /* runner: test edi, edi */
    B(0x75, 0x0E);                                                    /* jnz jobs */
    B(0xFF, 0x25, 0, 0, 0, 0); Q(game + 0xF3519Eu);                   /* jmp game+0xF3519E (no jobs) */
    B(0x4C, 0x89, 0xF1);                                              /* jobs: mov rcx, r14 */
    B(0x89, 0xFA);                                                    /* mov edx, edi */
    B(0x48, 0xB8); Q((uintptr_t)TileJobsInline);                      /* mov rax, TileJobsInline */
    B(0xFF, 0xD0);                                                    /* call rax */
    B(0xFF, 0x25, 0, 0, 0, 0); Q(game + 0xF34FF5u);                   /* jmp game+0xF34FF5 */
#undef B
#undef Q
    memcpy(stub, c, (size_t)n);
    DWORD old;
    if (!VirtualProtect(stub, 0x100, PAGE_EXECUTE_READ, &old)) return 0;
    uint8_t patch[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
    int64_t rel = (int64_t)(uintptr_t)stub - (int64_t)(game + TILE_SUBMIT_SITE + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) return 0;
    int32_t r32 = (int32_t)rel; memcpy(patch + 1, &r32, 4);
    if (!VirtualProtect((void*)(game + TILE_SUBMIT_SITE), 7, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void*)(game + TILE_SUBMIT_SITE), patch, 7);
    VirtualProtect((void*)(game + TILE_SUBMIT_SITE), 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)(game + TILE_SUBMIT_SITE), 7);
    done = 1;
    return 1;
}

/* Stage B: a runner's dispatcher calls (the tile build ops, 0x4E/0x26) write nothing shared (static, hellforge-1a,
 * ~/xl-work/static/loadtime/STAGE_B_RECIPES.md):
 *  - the tail at 0xC19F2B (test r12d, r12d; jne 0xC19F4D: the seed text into game+0x37CEDE0 by 0xFC3E20, then the
 *    telemetry append 0xBF4F20) jumps to 0xC19F8E (the cookie check and the epilogue) on a runner; r11 is dead there on
 *    every path, rax is the return value on one, flags are set again;
 *  - each case's crash-context label ([api+0x10]+0x470 = exe 0x323E70 -> 0x742B0, a string map without a lock that
 *    reallocates) returns at once on a runner (the interface slot swapped, as TerrainSampleInstall swaps +0x1F0). */
typedef void (*CrashLabelFn)(const char* key, const char* value);
static CrashLabelFn g_crashLabel;
static void CrashLabelThunk(const char* key, const char* value)
{
    if (TileRunnerSelf() != NULL) return;
    g_crashLabel(key, value);
}
static int TileDispatchInstall(void)
{
    static int done = -1;
    if (done >= 0) return done;
    done = 0;
    uintptr_t game = g_world.game, exe = (uintptr_t)GetModuleHandleW(NULL);
    uint64_t api = 0, table = 0, slotValue = 0;
    if (!ApiCodeMatches(game + 0xC19F2Bu, "45 85 E4 75 1D 49 8B 4F 18") || !ApiCodeMatches(exe + 0x323E70u, "4C 8B C2 48 8B D1 48 8B 0D 8B C3 6E 01 E9 2E 04 D5 FF") ||
        SafeRead(game + 0x3326308u, &api, 8) || api == 0 || SafeRead(api + 0x10, &table, 8) || table == 0 || SafeRead(table + 0x470, &slotValue, 8) ||
        slotValue != exe + 0x323E70u)
    { Note("difficulty: stamps: the dispatcher's tail or the crash label is not the reviewed one; the generator builds the tiles"); return 0; }
    uint8_t* stub = AllocateNearGame(game, 0x100);
    if (stub == NULL) return 0;
    uint8_t c[64]; int n = 0;
    uint64_t v = (uint64_t)(uintptr_t)&g_tileRunnerTls;
    c[n++] = 0x49; c[n++] = 0xBB; memcpy(c + n, &v, 8); n += 8;                  /* 00 mov r11, &g_tileRunnerTls */
    const uint8_t mid[] = {0x45, 0x8B, 0x1B,                                      /* 0A mov r11d, [r11] */
                           0x41, 0x83, 0xFB, 0x40,                                /* 0D cmp r11d, 64 */
                           0x73, 0x0E,                                            /* 11 jae 0x21 */
                           0x65, 0x4E, 0x8B, 0x1C, 0xDD, 0x80, 0x14, 0x00, 0x00,  /* 13 mov r11, gs:[r11*8+0x1480] */
                           0x4D, 0x85, 0xDB,                                      /* 1C test r11, r11 */
                           0x75, 0x0E,                                            /* 1F jnz 0x2F */
                           0x45, 0x85, 0xE4,                                      /* 21 test r12d, r12d */
                           0x0F, 0x85, 0, 0, 0, 0,                                /* 24 jne game+0xC19F4D (rel at 26) */
                           0xE9, 0, 0, 0, 0,                                      /* 2A jmp game+0xC19F30 (rel at 2B) */
                           0xE9, 0, 0, 0, 0};                                     /* 2F jmp game+0xC19F8E (rel at 30) */
    memcpy(c + n, mid, sizeof mid); n += (int)sizeof mid;
    const struct { int at, end; uintptr_t to; } rels[3] = {{0x26, 0x2A, game + 0xC19F4Du}, {0x2B, 0x2F, game + 0xC19F30u}, {0x30, 0x34, game + 0xC19F8Eu}};
    for (int k = 0; k < 3; k++)
    {
        int64_t rel = (int64_t)rels[k].to - (int64_t)((uintptr_t)stub + (uintptr_t)rels[k].end);
        if (rel < INT32_MIN || rel > INT32_MAX) return 0;
        int32_t r32 = (int32_t)rel; memcpy(c + rels[k].at, &r32, 4);
    }
    memcpy(stub, c, (size_t)n);
    DWORD old;
    if (!VirtualProtect(stub, 0x100, PAGE_EXECUTE_READ, &old)) return 0;
    /* the label thunk first (it only filters runners), then the tail jump */
    void** slot = (void**)(uintptr_t)(table + 0x470);
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) return 0;
    g_crashLabel = (CrashLabelFn)*slot;
    InterlockedExchangePointer(slot, (void*)CrashLabelThunk);
    VirtualProtect(slot, 8, old, &old);
    uint8_t patch[5] = {0xE9, 0, 0, 0, 0};
    int64_t rel = (int64_t)(uintptr_t)stub - (int64_t)(game + 0xC19F2Bu + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) return 0;
    int32_t r32 = (int32_t)rel; memcpy(patch + 1, &r32, 4);
    if (!VirtualProtect((void*)(game + 0xC19F2Bu), 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void*)(game + 0xC19F2Bu), patch, 5);
    VirtualProtect((void*)(game + 0xC19F2Bu), 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)(game + 0xC19F2Bu), 5);
    done = 1;
    return 1;
}

/* HELLFORGE_XL_TILE_BUILDS=0: the generator builds the tiles (stage A) */
static int TileBuildsOn(void)
{
    static int on = -1;
    if (on < 0) { char v[8] = ""; on = !(GetEnvironmentVariableA("HELLFORGE_XL_TILE_BUILDS", v, sizeof v) > 0 && v[0] == '0'); }
    return on;
}

static int TileThreads(void)
{
    static int n = -1;
    if (n < 0)
    {
        char t[16] = "";
        n = 12;
        if (GetEnvironmentVariableA("HELLFORGE_XL_TILE_THREADS", t, sizeof t) > 0) n = atoi(t);
        if (n < 1) n = 1;
        if (n > TILE_RUNNERS_MAX - 1) n = TILE_RUNNERS_MAX - 1; /* the last runner is difficulty_world.c's EarlyPackages' */
    }
    if (g_tileRunnerTls == TLS_OUT_OF_INDEXES) { DWORD k = TlsAlloc(); if (k != TLS_OUT_OF_INDEXES) g_tileRunnerTls = k; }
    if (n > 1 && !g_tileJobsBroken && TileJobsInstall()) return n;
    return 1;
}

/* A runner's engine job: r9 = its element of the submitted array, the runner. */
static uint64_t TileRunnerJob(uint64_t a, uint64_t b, uint64_t c, uint64_t runner)
{
    TileRunner* r = (TileRunner*)(uintptr_t)runner;
    TlsSetValue(g_tileRunnerTls, r);
    if (!g_tileStop && g_tileWork != NULL) g_tileWork(r);
    TlsSetValue(g_tileRunnerTls, NULL);
    InterlockedIncrement(&g_tileFinished);
    return 0;
}

/* `count` runners (ids 1..count) as engine jobs on the regular workers: [[game+0x33263E8]+0x88](fn, the array, name,
 * r9d 1, arg5 3, count, 0), queue 1 + 3 x 3 = 10, which the regular workers pop (the design notes: exe
 * 0x6261C9..0x6261D6, 0x626A41/0x626A98); the job is called (a, b, c, element). The number submitted (0: none). */
static int TileRunnersStart(int count)
{
    static uint64_t array[TILE_RUNNERS_MAX];
    static const char name[] = "hellforge stamp tile runner";
    int n = 0;
    for (int i = 1; i <= count && n < TILE_RUNNERS_MAX; i++)
    {
        TileRunner* r = TileRunnerMake(i);
        if (r == NULL) break;
        array[n++] = (uint64_t)(uintptr_t)r;
    }
    uint64_t api = 0, submit = 0;
    if (n == 0 || SafeRead(g_world.game + 0x33263E8u, &api, 8) || api == 0 || SafeRead(api + 0x88, &submit, 8) || submit == 0) return 0;
    typedef uint64_t (*SubmitFn)(void* fn, uint64_t* data, const char* name, uint32_t queue, uint32_t priority, uint32_t count, uint64_t zero);
    ((SubmitFn)(uintptr_t)submit)((void*)TileRunnerJob, array, name, 1, 3, (uint32_t)n, 0);
    return n;
}

/* After g_tileStop: every submitted runner has returned (a job the workers start late returns at once). */
static void TileRunnersJoin(int started)
{
    ULONGLONG since = GetTickCount64();
    while (g_tileFinished < started)
    {
        if (GetTickCount64() - since > 30000)
        {
            Note("difficulty: stamps: %ld of %d runners did not return within 30 s; no runners for the rest of the session", (long)g_tileFinished, started);
            g_tileJobsBroken = 1;
            return;
        }
        Sleep(1);
    }
}

/* A built slot's op 0x2B on runner `r` (jobs inline when `inlineJobs`, with the runner in the thread's TLS slot), then
 * its candidates into the slot (the serial code's collection, per tile). */
static void StampTile2B(TileRunner* r, StampSlot* s, int inlineJobs)
{
    const float core = (float)TERRAIN_TILE_CORE, margin = (float)TERRAIN_TILE_MARGIN, extent = (float)TERRAIN_TILE_EXTENT;
    LARGE_INTEGER qf, q0, q1; QueryPerformanceFrequency(&qf);
    uint8_t* T = s->T;
    const int t = s->t;
    const float ox = s->ox, oy = s->oy;
    /* the run's own index, the widened gates (settings2B), no thinning (the section's below[]), the rules' picks scaled
     * to the tile's items */
    uintptr_t index = StampIndex(r, g_stampPar.P, T, extent);
    if (index == 0) return;
    s->indexed = 1;
    TileRunnerContext(r, g_stampPar.ctx, g_stampPar.settings2B, index);
    memcpy(r->one, g_stampPar.rec91Copy, sizeof r->one);
    WorldTileRelocate(r->one, g_stampPar.gen, (uintptr_t)T);
    { uint64_t v; memcpy(&v, r->one + 0x1990, 8); v ^= TerrainSeed(t) ^ 0x5354414D50ull; memcpy(r->one + 0x1990, &v, 8); }
    uint64_t emptyBefore = r->emptyQueries;
    if (inlineJobs) { r->scale = s->scale; OverlapRunnerTileStart(r); }
    else { StampRulesScale(T, s->items, s->scale); InterlockedIncrement(&g_worldOpRun); }
    QueryPerformanceCounter(&q0);
    volatile uint32_t at = 0;
    volatile int calls = 0;
    uint8_t phase = 0;
    LONG fault;
    if (inlineJobs) { fault = WorldTileGuardedCall(g_world.game + 0xF34CC0u, r->one, (uint64_t)(uintptr_t)r->ctx); at = 1; }
    else fault = WorldTileGuarded(r->one, (uint64_t)(uintptr_t)r->ctx, &at, &phase, g_stampPar.g, g_stampPar.h, 100000, &calls);
    if (!fault && at != 0) WorldArenaReset((uint64_t)(uintptr_t)r->ctx);
    QueryPerformanceCounter(&q1); s->t2B = (double)(q1.QuadPart - q0.QuadPart) / (double)qf.QuadPart;
    if (!inlineJobs) { InterlockedIncrement(&g_worldOpRun); StampRulesRestore(); }
    s->emptyQueries = r->emptyQueries - emptyBefore;
    if (fault || at == 0) return;
    s->ran = 1;
    uint32_t n = 0; memcpy(&n, T + 0x11A7108, 4);
    if (n > 8192) n = 8192;
    s->made = n;
    QueryPerformanceCounter(&q0);
    const float world = g_stampPar.world, half = g_stampPar.half, cgx = g_stampPar.cgx, cgy = g_stampPar.cgy;
    const float* rings = g_stampPar.rings;
    for (uint32_t i = 0; i < n; i++)
    {
        const uint8_t* loc = T + 0x11A7280 + (size_t)i * 0xE0;
        float xy[2], box[4]; uint32_t type = 0; uint16_t flags = 0;
        memcpy(xy, loc, 8); memcpy(box, loc + 0xB0, 16); memcpy(&type, loc + 0xD8, 4); memcpy(&flags, loc + 0xD4, 2);
        if (xy[0] < margin || xy[0] >= margin + core || xy[1] < margin || xy[1] >= margin + core) { s->why[0]++; continue; }
        int known = 0;
        for (uint32_t q = 0; q < g_stampPar.typeCount && !known; q++) known = g_stampPar.types[q] == type;
        if (!known) { s->why[1]++; continue; }
        float gx = xy[0] + ox, gy = xy[1] + oy, d = sqrtf((gx - half) * (gx - half) + (gy - half) * (gy - half));
        float lo[2] = {fminf(box[0], box[2]) + ox, fminf(box[1], box[3]) + oy}, hi[2] = {fmaxf(box[0], box[2]) + ox, fmaxf(box[1], box[3]) + oy};
        if (d >= world * g_stampPar.s5C * 0.5f || (d >= world * g_stampPar.s54 * 0.5f && !(flags & 1)) || lo[0] < 0.02f * world || lo[1] < 0.02f * world ||
            hi[0] > 0.98f * world || hi[1] > 0.98f * world) { s->why[2]++; continue; }
        float reach = fmaxf(fabsf(gx - cgx), fabsf(gy - cgy)), side = fmaxf(hi[0] - lo[0], hi[1] - lo[1]);
        uint32_t ring = reach <= rings[0] ? 0 : reach <= rings[1] ? 1 : reach <= rings[2] ? 2 : 3;
        uint32_t hash = (uint32_t)t * 0x9E3779B1u ^ (i + 1) * 0x85EBCA6Bu; hash ^= hash >> 15; hash *= 0xC2B2AE35u; hash ^= hash >> 13;
        { int bin = side < 4 ? 0 : side < 8 ? 1 : side < 16 ? 2 : side < 32 ? 3 : side < 64 ? 4 : side < 128 ? 5 : side < 256 ? 6 : 7; s->sides[ring][bin]++; }
        /* no thinning: what the level does not hold, the plan does */
        if (s->count >= STAMP_TILE_CANDIDATES) { s->why[4]++; continue; }
        StampCandidate* c = &s->cand[s->count++];
        memcpy(c->loc, loc, 0xE0);
        memcpy(c->extra, T + DIF_LOCATIONS_B_NEW + (size_t)i * 0x40, 0x40);
        float w2[2] = {gx, gy}, b4[4] = {box[0] + ox, box[1] + oy, box[2] + ox, box[3] + oy};
        memcpy(c->loc, w2, 8); memcpy(c->loc + 0xB0, b4, 16);
        memcpy(c->extra + 0x10, &g_stampPar.rec91, 8); memset(c->extra + 0x38, 0, 4);
        c->side = side; c->ring = ring; c->hash = hash;
        c->prio = ring == 0 ? side : side / fmaxf(reach, 1.0f); /* beyond ring 0: by the size it shows from the centre */
    }
    QueryPerformanceCounter(&q1); s->tCollect = (double)(q1.QuadPart - q0.QuadPart) / (double)qf.QuadPart;
}

/* The built slot to run next: the one earliest in order[] (the generator commits in that order). */
static StampSlot* StampTakeBuilt(void)
{
    StampSlot* best = NULL;
    for (int i = 0; i < g_stampPar.slotCount; i++)
    {
        StampSlot* s = &g_stampPar.slots[i];
        if (s->state == STAMP_SLOT_BUILT && (best == NULL || s->k < best->k)) best = s;
    }
    if (best != NULL && InterlockedCompareExchange(&best->state, STAMP_SLOT_RUNNING, STAMP_SLOT_BUILT) != STAMP_SLOT_BUILT) return NULL;
    return best;
}

/* A runner on an engine worker (TileRunnersStart): built slots until the section stops. */
/* Stage B: one whole tile on runner `r` (its TLS slot set): a free slot first, then the next tile of order[] (so the
 * tile the generator commits next always has a slot), built, 0x4E/0x26, op 0x2B. 0: no tile left; -1: no free slot. */
static int StampRunnerTile(TileRunner* r)
{
    StampSlot* s = NULL;
    for (int i = 0; i < g_stampPar.slotCount && s == NULL; i++)
        if (g_stampPar.slots[i].state == STAMP_SLOT_FREE && InterlockedCompareExchange(&g_stampPar.slots[i].state, STAMP_SLOT_RUNNING, STAMP_SLOT_FREE) == STAMP_SLOT_FREE)
            s = &g_stampPar.slots[i];
    if (s == NULL) return g_stampPar.nextTile >= g_stampPar.tiles ? 0 : -1;
    LONG k = InterlockedIncrement(&g_stampPar.nextTile) - 1;
    if (k >= g_stampPar.tiles) { InterlockedExchange(&s->state, STAMP_SLOT_FREE); return 0; }
    s->k = (int)k; s->t = g_stampPar.order[k];
    MemoryBarrier();
    InterlockedExchange(&g_stampPar.slotOf[k], (LONG)(s - g_stampPar.slots));
    StampTileBuild(r, s);
    if (s->fixed) StampTile2B(r, s, 1);
    InterlockedExchange(&s->state, STAMP_SLOT_DONE);
    return 1;
}

static void StampRunnerLoop(TileRunner* r)
{
    while (!g_tileStop)
    {
        if (g_stampPar.stageB)
        {
            int got = StampRunnerTile(r);
            if (got == 0) break;
            if (got < 0) Sleep(1);
            continue;
        }
        StampSlot* s = StampTakeBuilt();
        if (s == NULL) { Sleep(1); continue; }
        StampTile2B(r, s, 1);
        InterlockedExchange(&s->state, STAMP_SLOT_DONE);
    }
}

/* After the level's op 0x2B has finished (difficulty_world.c's WorldDispatch). */
static void TerrainStamps(uint64_t records, uint32_t end, uint32_t at91, uint64_t context, uint64_t g, uint64_t h, uintptr_t gen)
{
    if (g_world.below != NULL && g_world.current > 1.0f)
    {
        uint32_t count = 0; SafeRead(gen + 0x11A7108u, &count, 4);
        Note("difficulty: extra large maps: after the level's op 0x2B: %u locations (limit %u); %u instances refused at the limit", count,
            (unsigned)DIF_PLACE_LIMIT, (unsigned)g_world.below[3]);
    }
    if (!StampsOn() || g_world.current != 8.0f || g_world.below == NULL) return;
    if (g_terrain.level != g_world.levels || g_terrain.columns == 0 || g_terrain.block == NULL || g_terrain.tmpl == NULL)
    { Note("difficulty: stamps: the level's tiles are not built; no stamp rings"); return; }
    LARGE_INTEGER hz, t0, t1;
    QueryPerformanceFrequency(&hz); QueryPerformanceCounter(&t0);
    uintptr_t settings = 0, P = 0, levelIndex = 0;
    if (SafeRead(context + 8, &settings, 8) || settings == 0 || SafeRead(context + 0x20, &P, 8) || P == 0 ||
        SafeRead(g_worldLevel + 0x2C0, &levelIndex, 8) || levelIndex != P)
    { Note("difficulty: stamps: the context's stamp index is not the level's (0x%llX); no stamp rings", (unsigned long long)P); return; }
    uint32_t W = 0; float s50 = 0, s54 = 0, s5C = 0;
    SafeRead(settings + 0x2C, &W, 4); SafeRead(settings + 0x50, &s50, 4); SafeRead(settings + 0x54, &s54, 4); SafeRead(settings + 0x5C, &s5C, 4);
    const float world = (float)W, half = world * 0.5f;
    /* the type tables of 0x2B and 0xA5 must be the same for the package test */
    uint64_t rec91 = records + (uint64_t)at91 * 0x19B0u, table91 = 0, tableA5 = 0;
    SafeRead(rec91 + 8 + 0x48, &table91, 8);
    for (uint32_t i = 0; i < end && tableA5 == 0; i++)
    {
        uint32_t op = 0;
        if (!SafeRead(records + (uint64_t)i * 0x19B0u, &op, 4) && op == 0xA5) SafeRead(records + (uint64_t)i * 0x19B0u + 8, &tableA5, 8);
    }
    if (table91 == 0 || table91 != tableA5) { Note("difficulty: stamps: 0x2B's type table 0x%llX is not 0xA5's 0x%llX; no stamp rings", (unsigned long long)table91, (unsigned long long)tableA5); return; }
    uint32_t types[256], typeCount = 0;
    {
        uint32_t n = 0; SafeRead(gen + 0x11A7080u, &n, 4);
        for (uint32_t k = 0; k < n && typeCount < 256; k++) if (!SafeRead(gen + 0x18008u + 4ull * k, &types[typeCount], 4)) typeCount++;
    }
    uint32_t clearingCount = 0; SafeRead(gen + 0x11A7244u, &clearingCount, 4);
    if (clearingCount > 4096) clearingCount = 0;
    /* the centre: the squad's drop point on the war board (as the near tile reads it), else the world's centre */
    {
        uint64_t board = 0; uint8_t flag = 0; float p[2] = {0, 0};
        int have = !SafeRead(g_world.game + 0x347CEE8u, &board, 8) && board != 0 && !SafeRead(board + 0x17A2B0u, &flag, 1) && flag == 1 &&
            !SafeRead(board + 0x17A2B4u, &p[0], 4) && !SafeRead(board + 0x17A2B8u, &p[1], 4) && isfinite(p[0]) && isfinite(p[1]) &&
            fabsf(p[0]) <= 7000.0f && fabsf(p[1]) <= 7000.0f;
        if (!have) p[0] = p[1] = 0.0f;
        g_stamps.centre[0] = 256.0f * floorf(p[0] / 256.0f + 0.5f); g_stamps.centre[1] = 256.0f * floorf(p[1] / 256.0f + 0.5f);
        g_stamps.level = g_world.levels;
        Note("difficulty: stamps: centre (%.0f, %.0f) (%s); world %u, settings +0x50 %.3f +0x54 %.3f +0x5C %.3f; %u level types, %u clearings, 0x26 %s, 0x4E %s",
            (double)g_stamps.centre[0], (double)g_stamps.centre[1], have ? "the war board's drop point" : "no drop point: the world's centre", W, (double)s50,
            (double)s54, (double)s5C, typeCount, clearingCount, g_stamps.rec26Level == g_world.levels ? "kept" : "MISSING", g_stamps.rec4ELevel == g_world.levels ? "kept" : "MISSING");
    }
    float rings[6]; StampRings(rings);
    const float core = (float)TERRAIN_TILE_CORE, extent = (float)TERRAIN_TILE_EXTENT;
    const float cgx = g_stamps.centre[0] + half, cgy = g_stamps.centre[1] + half; /* generator metres */
    /* every tile of the playable disc, nearest first: the map's whole plan (the user's choice, 2026-11-05: plan at load,
     * stream near the players). What the generation's room holds goes into the level; the rest is the plan the stamp
     * window (difficulty_stream_window.c) puts in near the players. */
    int order[1024], tiles = 0;
    for (int t = 0; t < g_terrain.tiles && tiles < 1024; t++)
        if (TerrainTileWanted(t)) order[tiles++] = t;
    for (int a = 1; a < tiles; a++) /* by the Chebyshev distance of the tile's centre */
    {
        int t = order[a], b = a;
        float da = fmaxf(fabsf((float)(t % g_terrain.columns) * core + core / 2 - cgx), fabsf((float)(t / g_terrain.columns) * core + core / 2 - cgy));
        while (b > 0)
        {
            int u = order[b - 1];
            float du = fmaxf(fabsf((float)(u % g_terrain.columns) * core + core / 2 - cgx), fabsf((float)(u / g_terrain.columns) * core + core / 2 - cgy));
            if (du <= da) break;
            order[b] = u; b--;
        }
        order[b] = t;
    }
    if (g_stamps.cand == NULL)
    {
        g_stamps.reserved = STAMP_PLAN_MAX;
        g_stamps.cand = (StampCandidate*)VirtualAlloc(NULL, sizeof(StampCandidate) * (size_t)g_stamps.reserved, MEM_RESERVE, PAGE_READWRITE);
        g_stamps.capacity = 0;
    }
    if (g_stamps.cand == NULL) { Note("difficulty: stamps: no memory"); return; }
    g_stamps.count = 0;
    g_plan.level = -1; g_plan.count = 0;
    uint32_t built = 0, ran = 0, made = 0, why[8] = {0}; /* why: core, type, gate, ring, room */
    uint32_t sides[4][8] = {{0}};
    /* where the planning's time goes (load time): the tile build, 0x4E/0x26 (generator thread), the tiles' op 0x2B and
     * the collection (summed over the runners); then the sort, the level's clash tests and the plan */
    LARGE_INTEGER qf, q0, q1; QueryPerformanceFrequency(&qf);
    double tBuild = 0, tFix = 0, t2B = 0, tCollect = 0, tSort = 0, tClash = 0, tTiles = 0;
#define STAMP_T0() QueryPerformanceCounter(&q0)
#define STAMP_T1(acc) do { QueryPerformanceCounter(&q1); (acc) += (double)(q1.QuadPart - q0.QuadPart) / (double)qf.QuadPart; } while (0)
    /* the section's constants for the runners */
    g_stampPar.records = records; g_stampPar.end = end; g_stampPar.context = context; g_stampPar.g = g; g_stampPar.h = h; g_stampPar.gen = gen;
    g_stampPar.P = P; g_stampPar.rec91 = rec91; g_stampPar.table91 = table91;
    memcpy(g_stampPar.types, types, sizeof types); g_stampPar.typeCount = typeCount; g_stampPar.clearingCount = clearingCount;
    memcpy(g_stampPar.rings, rings, sizeof rings); g_stampPar.world = world; g_stampPar.half = half; g_stampPar.s54 = s54; g_stampPar.s5C = s5C;
    g_stampPar.cgx = cgx; g_stampPar.cgy = cgy;
    if (SafeRead(context, g_stampPar.ctx, sizeof g_stampPar.ctx) || SafeRead(settings, g_stampPar.settings2B, sizeof g_stampPar.settings2B) ||
        SafeRead(rec91, g_stampPar.rec91Copy, sizeof g_stampPar.rec91Copy))
    { Note("difficulty: stamps: the level's context, settings or record 91 did not read; no stamp rings"); return; }
    { uint32_t e = (uint32_t)extent; float four = 4.0f; memcpy(g_stampPar.settings2B + 0x2C, &e, 4); memcpy(g_stampPar.settings2B + 0x50, &four, 4);
      memcpy(g_stampPar.settings2B + 0x54, &four, 4); memcpy(g_stampPar.settings2B + 0x5C, &four, 4); }
    /* Research (HELLFORGE_XL_TILE_COMPARE=1): the tiles run twice, one runner then the configured runners, and the two
     * candidate arrays are compared byte for byte (the parallel planning must give the serial result exactly) */
    static int compare = -1;
    if (compare < 0) { char v[8] = ""; compare = GetEnvironmentVariableA("HELLFORGE_XL_TILE_COMPARE", v, sizeof v) > 0 && v[0] == '1'; }
    StampCandidate* serial = NULL; uint32_t serialCount = 0;
    for (int pass = 0; pass < (compare ? 2 : 1); pass++)
    {
    if (pass == 1)
    {
        g_stamps.count = 0; built = ran = made = 0; memset(why, 0, sizeof why); memset(sides, 0, sizeof sides);
        tBuild = tFix = t2B = tCollect = 0;
    }
    /* runners and slots: runner 0 is the generator's; slot 0 is g_terrain.block */
    int R = TileThreads();
    if (compare && pass == 0) R = 1;
    static uint8_t* slotBlock[STAMP_SLOTS_MAX];
    static StampCandidate* slotCand[STAMP_SLOTS_MAX];
    int slots = R > 1 ? R + 2 : 1;
    if (slots > STAMP_SLOTS_MAX) slots = STAMP_SLOTS_MAX;
    TileRunner* mine = TileRunnerMake(0);
    if (mine == NULL) { Note("difficulty: stamps: no memory for the planning's runner; no stamp rings"); return; }
    int have = 0;
    for (; have < slots; have++)
    {
        if (have > 0 && slotBlock[have] == NULL) slotBlock[have] = (uint8_t*)VirtualAlloc(NULL, GEN_BLOCK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (slotCand[have] == NULL) slotCand[have] = (StampCandidate*)VirtualAlloc(NULL, sizeof(StampCandidate) * STAMP_TILE_CANDIDATES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if ((have > 0 && slotBlock[have] == NULL) || slotCand[have] == NULL) break;
        StampSlot* sl = &g_stampPar.slots[have];
        sl->T = have == 0 ? g_terrain.block : slotBlock[have]; sl->cand = slotCand[have]; sl->state = STAMP_SLOT_FREE;
    }
    if (have == 0) { Note("difficulty: stamps: no memory for the planning's slots; no stamp rings"); return; }
    g_stampPar.slotCount = have;
    if (have < 2) R = 1;
    g_stampPar.inlineJobs = R > 1;
    g_tileStop = 0; g_tileFinished = 0; g_tileWork = StampRunnerLoop;
    /* for the whole section: no thinning; a tile run's jobs make no per-job line; the overlap grid's runs (one per tile
     * run below when the game runs the jobs, the runners' own otherwise) */
    uint32_t below[2] = {g_world.below[0], g_world.below[1]};
    g_world.below[0] = g_world.below[1] = 0xFFFFFFFFu;
    g_stampTileRun = 1;
    InterlockedIncrement(&g_worldOpRun);
    /* stage B: the runners build too. The section holds the level's tile marker (lg+0x2D7C = S+0x5C, its low bit
     * flipped) and lattice spacing ([[lg+0x2D0]+0x68] = 16) for every tile; the runners' build settings are the level's
     * as they are then (TerrainBuild writes its four values into the runner's copy), 0x4E/0x26's the level's before */
    g_stampPar.stageB = 0;
    uintptr_t sectionLg = 0, spacingAt = 0; uint32_t sectionDisk = 0; float sectionSpacing = 0;
    if (R > 1 && TileBuildsOn() && TileDispatchInstall() && !SafeRead(context + 0x18, &sectionLg, 8) && sectionLg != 0 && settings == sectionLg + 0x2D20 &&
        !SafeRead(sectionLg + 0x2D0, &spacingAt, 8) && spacingAt != 0 && !SafeRead(spacingAt + 0x68, &spacingAt, 8) && spacingAt != 0 &&
        !SafeRead(sectionLg + 0x2D7C, &sectionDisk, 4) && !SafeRead(spacingAt, &sectionSpacing, 4) &&
        !SafeRead(settings, g_stampPar.levelS, sizeof g_stampPar.levelS))
    {
        TerrainTileZonesPrepare(gen);
        *(volatile uint32_t*)(sectionLg + 0x2D7C) = sectionDisk ^ 1u;
        *(volatile float*)spacingAt = 16.0f;
        g_tileSectionHeld = 1;
        memcpy(g_stampPar.buildS, g_stampPar.levelS, sizeof g_stampPar.buildS);
        { uint32_t flipped = sectionDisk ^ 1u; memcpy(g_stampPar.buildS + 0x5C, &flipped, 4); }
        g_stampPar.stageB = 1;
    }
    g_stampPar.tiles = tiles; g_stampPar.nextTile = 0;
    memcpy(g_stampPar.order, order, sizeof(int) * (size_t)tiles);
    for (int k = 0; k < tiles; k++) g_stampPar.slotOf[k] = -1;
    volatile LONG* slotOf = g_stampPar.slotOf;
    int started = R > 1 ? TileRunnersStart(R - 1) : 0;
    int next = 0, commit = 0, selfRuns = 0, stallNoted = 0;
    ULONGLONG stallSince = 0;
    uint32_t noIndex = 0, notFixed = 0, noRun = 0;
    uint64_t emptyQueries = 0;
    LARGE_INTEGER w0, w1; QueryPerformanceCounter(&w0);
    if (!g_stampPar.inlineJobs) TlsSetValue(g_tileRunnerTls, NULL);
    while (commit < tiles)
    {
        int progress = 0;
        while (commit < tiles && slotOf[commit] >= 0 && g_stampPar.slots[slotOf[commit]].state == STAMP_SLOT_DONE)
        {
            StampSlot* sl = &g_stampPar.slots[slotOf[commit]];
            built += (uint32_t)sl->built;
            if (sl->built && !sl->fixed) { notFixed++; if (notFixed <= 8) Note("difficulty: stamps: tile %d: 0x4E/0x26 did not run", sl->t); }
            if (sl->fixed && !sl->indexed) { noIndex++; if (noIndex <= 8) Note("difficulty: stamps: no index for tile %d", sl->t); }
            if (sl->indexed && !sl->ran) { noRun++; if (noRun <= 8) Note("difficulty: stamps: tile %d: op 0x2B did not run", sl->t); }
            ran += (uint32_t)sl->ran; made += sl->made;
            for (int q = 0; q < 8; q++) why[q] += sl->why[q];
            for (int a = 0; a < 4; a++) for (int b = 0; b < 8; b++) sides[a][b] += sl->sides[a][b];
            t2B += sl->t2B; tCollect += sl->tCollect; tBuild += sl->tBuild; tFix += sl->tFix; emptyQueries += sl->emptyQueries;
            for (uint32_t i = 0; i < sl->count; i++)
            {
                if (g_stamps.count >= g_stamps.capacity && !StampGrow()) { why[4] += sl->count - i; break; }
                g_stamps.cand[g_stamps.count++] = sl->cand[i];
            }
            InterlockedExchange(&sl->state, STAMP_SLOT_FREE);
            commit++; progress = 1;
        }
        if (g_stampPar.stageB)
        {
            if (!progress && g_stampPar.nextTile < tiles)
            {
                TlsSetValue(g_tileRunnerTls, mine);
                int got = StampRunnerTile(mine);
                TlsSetValue(g_tileRunnerTls, NULL);
                if (got > 0) { selfRuns++; progress = 1; }
            }
            if (!progress)
            {
                ULONGLONG now = GetTickCount64();
                if (stallSince == 0) stallSince = now;
                else if (now - stallSince > 10000 && !stallNoted) { stallNoted = 1; TileRunnersStallNote("stamps", started); }
                SwitchToThread();
            }
            else stallSince = 0;
            continue;
        }
        if (next < tiles)
        {
            StampSlot* sl = NULL;
            for (int i = 0; i < g_stampPar.slotCount && sl == NULL; i++) if (g_stampPar.slots[i].state == STAMP_SLOT_FREE) sl = &g_stampPar.slots[i];
            if (sl != NULL)
            {
                sl->k = next; sl->t = order[next];
                slotOf[next] = (LONG)(sl - g_stampPar.slots);
                next++;
                StampTileBuild(NULL, sl);
                InterlockedExchange(&sl->state, sl->fixed ? STAMP_SLOT_BUILT : STAMP_SLOT_DONE);
                continue;
            }
        }
        if (!progress)
        {
            StampSlot* sl = StampTakeBuilt();
            if (sl != NULL)
            {
                if (g_stampPar.inlineJobs) TlsSetValue(g_tileRunnerTls, mine);
                StampTile2B(mine, sl, g_stampPar.inlineJobs);
                if (g_stampPar.inlineJobs) TlsSetValue(g_tileRunnerTls, NULL);
                InterlockedExchange(&sl->state, STAMP_SLOT_DONE);
                selfRuns++; progress = 1;
            }
        }
        if (!progress) SwitchToThread();
    }
    g_tileStop = 1;
    TileRunnersJoin(started);
    if (g_stampPar.stageB)
    {
        *(volatile uint32_t*)(sectionLg + 0x2D7C) = sectionDisk;
        *(volatile float*)spacingAt = sectionSpacing;
        g_tileSectionHeld = 0;
    }
    QueryPerformanceCounter(&w1); tTiles = (double)(w1.QuadPart - w0.QuadPart) / (double)qf.QuadPart;
    InterlockedIncrement(&g_worldOpRun);
    g_stampTileRun = 0;
    g_world.below[0] = below[0]; g_world.below[1] = below[1];
    g_emptyIndexQueries += (LONG64)emptyQueries;
    /* the slots' blocks and candidates and the runners' memory go (0.5-1 GB touched; the load's peak is later) */
    if (!g_tileJobsBroken)
    {
        for (int i = 1; i < STAMP_SLOTS_MAX; i++) if (slotBlock[i] != NULL) { VirtualFree(slotBlock[i], 0, MEM_RELEASE); slotBlock[i] = NULL; }
        for (int i = 0; i < STAMP_SLOTS_MAX; i++) if (slotCand[i] != NULL) { VirtualFree(slotCand[i], 0, MEM_RELEASE); slotCand[i] = NULL; }
        TileRunnersRelease();
    }
    Note("difficulty: stamps: the tiles ran on %d runners (%d engine jobs started, %d slots; %s; the generator ran %d of %d tiles' %s itself) in %.1f s",
        R, started, g_stampPar.slotCount, g_stampPar.stageB ? "the runners built them" : "the generator built them", selfRuns, tiles,
        g_stampPar.stageB ? "build and op 0x2B" : "op 0x2B", tTiles);
    if (compare && pass == 0 && g_stamps.count > 0)
    {
        serial = (StampCandidate*)VirtualAlloc(NULL, sizeof(StampCandidate) * (size_t)g_stamps.count, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (serial != NULL) { memcpy(serial, g_stamps.cand, sizeof(StampCandidate) * (size_t)g_stamps.count); serialCount = g_stamps.count; }
    }
    }
    if (serial != NULL)
    {
        uint32_t differ = 0, first = 0xFFFFFFFFu, n = serialCount < g_stamps.count ? serialCount : g_stamps.count;
        for (uint32_t i = 0; i < n; i++)
            if (memcmp(&serial[i], &g_stamps.cand[i], sizeof(StampCandidate)) != 0) { if (differ++ == 0) first = i; }
        if (differ == 0 && serialCount == g_stamps.count)
            Note("difficulty: stamps: compare: the runners' %u candidates are byte-identical to one runner's", serialCount);
        else
        {
            Note("difficulty: stamps: compare: DIFFERENT: one runner %u candidates, the runners %u; %u of the first %u differ (first at %u)",
                serialCount, g_stamps.count, differ, n, first);
            if (first < n)
            {
                const StampCandidate* x = &serial[first]; const StampCandidate* y = &g_stamps.cand[first];
                float a[2], b2[2]; uint32_t ta = 0, tb = 0; memcpy(a, x->loc, 8); memcpy(b2, y->loc, 8); memcpy(&ta, x->loc + 0xD8, 4); memcpy(&tb, y->loc + 0xD8, 4);
                Note("difficulty: stamps: compare: first difference: one runner (%.2f, %.2f) type %u hash 0x%X; the runners (%.2f, %.2f) type %u hash 0x%X",
                    (double)a[0], (double)a[1], ta, x->hash, (double)b2[0], (double)b2[1], tb, y->hash);
            }
        }
        VirtualFree(serial, 0, MEM_RELEASE);
    }
    /* into the level, ring 0 first (larger first), then the rest by apparent size (side / distance), while the table has
     * room; each tested against the level's own locations and roads in its index. Not against the stamps added here:
     * each tile's run already tested its own with the game's footprint test, and boxes of rotated footprints overlap far
     * more often than the footprints (run s70c: 4160 of the 4900 ring-0 stamps fell to a box test among themselves). */
    STAMP_T0();
    qsort(g_stamps.cand, g_stamps.count, sizeof *g_stamps.cand, StampCompare);
    STAMP_T1(tSort);
    STAMP_T0();
    uint32_t n0 = 0; SafeRead(gen + 0x11A7108u, &n0, 4);
    uint32_t n = n0, added[3] = {0}, blocked[2] = {0};
    uint64_t roads = 0, points = 0; SafeRead(P + 0x2058, &roads, 8); SafeRead(P + 0x2060, &points, 8);
    int roadsAreRoutes = roads == gen + g_routeEdges && points == gen + g_routePoints;
    StampKeyFn key = (StampKeyFn)(g_world.game + 0xC04860u);
    StampQueryFn query = (StampQueryFn)(g_world.game + 0xC05360u);
    StampInsertFn insert = (StampInsertFn)(g_world.game + 0xC04EF0u);
    uint64_t locTable = 0; SafeRead(P + 0x2050, &locTable, 8);
    static uint64_t hits[0x400];
    uint32_t planned = 0, planBlocked[2] = {0};
    for (uint32_t k = 0; k < g_stamps.count; k++)
    {
        StampCandidate* c = &g_stamps.cand[k];
        float box[4]; memcpy(box, c->loc + 0xB0, 16);
        float lo[2] = {fminf(box[0], box[2]), fminf(box[1], box[3])}, hi[2] = {fmaxf(box[0], box[2]), fmaxf(box[1], box[3])};
        uint32_t found = query(P + 8, key(P + 8, lo[0], lo[1]), key(P + 8, hi[0], hi[1]), hits, 0x400);
        int clash = 0;
        const uint32_t was[2] = {blocked[0], blocked[1]};
        for (uint32_t e = 0; e < found && e < 0x400 && !clash; e++)
        {
            uint16_t id = 0, fl = 0;
            if (SafeRead(hits[e], &id, 2) || SafeRead(hits[e] + 2, &fl, 2)) continue;
            if (!(fl & 1))
            {
                if (id >= n0) continue;
                float ob[4];
                if (locTable == 0 || SafeRead(locTable + 0xE0ull * id + 0xB0, ob, 16)) continue;
                float olo[2] = {fminf(ob[0], ob[2]), fminf(ob[1], ob[3])}, ohi[2] = {fmaxf(ob[0], ob[2]), fmaxf(ob[1], ob[3])};
                if (lo[0] < ohi[0] && hi[0] > olo[0] && lo[1] < ohi[1] && hi[1] > olo[1]) { clash = 1; blocked[0]++; }
            }
            else
            {
                float seg[4], width = 0;
                if (!roadsAreRoutes || id == 0 || SafeRead(points + 8ull * (id - 1u), seg, 16) || SafeRead(roads + 0x48ull * (fl >> 2) + 0x30, &width, 4)) { clash = 1; blocked[1]++; continue; }
                if (StampSegmentBox(seg, lo, hi) < 0.5f * width) { clash = 1; blocked[1]++; }
            }
        }
        if (clash) { if (n >= STAMP_LOCATION_LIMIT) { planBlocked[0] += blocked[0] - was[0]; planBlocked[1] += blocked[1] - was[1]; } continue; }
        if (n >= STAMP_LOCATION_LIMIT) /* the generation's room is full: into the plan (compacted to the front; planned <= k) */
        {
            if (planned != k) g_stamps.cand[planned] = *c;
            planned++;
            continue;
        }
        memcpy((void*)(uintptr_t)(gen + 0x11A7280u + 0xE0ull * n), c->loc, 0xE0);
        memcpy((void*)(uintptr_t)(gen + DIF_LOCATIONS_B_NEW + 0x40ull * n), c->extra, 0x40);
        uint8_t scratch[16];
        uint32_t* slot = insert(P + 8, lo[0], lo[1], hi[0], hi[1], scratch);
        if (slot != NULL) { *slot = n; (*(volatile uint32_t*)(uintptr_t)(P + 4))++; }
        g_stamps.ring[n] = (uint8_t)c->ring;
        n++; added[c->ring]++;
    }
    STAMP_T1(tClash);
    Note("difficulty: stamps: planning time: tiles %.1f s (tile builds %.1f s, 0x4E/0x26 %.1f s; op 0x2B %.1f s, collecting %.1f s summed over the runners), sort %.1f s, the level's clash tests %.1f s; %lld queries of an empty tile index answered without a scan",
        tTiles, tBuild, tFix, t2B, tCollect, tSort, tClash, (long long)g_emptyIndexQueries);
#undef STAMP_T0
#undef STAMP_T1
    *(volatile uint32_t*)(uintptr_t)(gen + 0x11A7108u) = n;
    *(volatile uint32_t*)(uintptr_t)(gen + 0x11A710Cu) = n;
    g_stamps.first = n0; g_stamps.end = n; g_stamps.ringLevel = g_world.levels;
    StampPlanBuild(planned, world);
    QueryPerformanceCounter(&t1);
    Note("difficulty: stamps: the plan holds %u stamps beyond the level's room (%u of them blocked by level locations, %u by roads were left out) in %d x %d buckets of %.0f m, %.0f MB",
        g_plan.count, planBlocked[0], planBlocked[1], STAMP_PLAN_GRID, STAMP_PLAN_GRID, (double)(world / STAMP_PLAN_GRID),
        (double)((double)g_plan.count * (sizeof(StampCandidate) + 2) / 1048576.0));
    Note("difficulty: stamps: %d tiles in reach, %u built, %u ran 0x2B (%u stamps); kept %u (dropped: %u outside the core, %u type not loaded, %u world gates, %u ring), %u blocked by level locations, %u by roads (roads %s); locations %u -> %u (ring 0 %u, 1 %u, 2 %u) in %.1f s",
        tiles, built, ran, made, g_stamps.count, why[0], why[1], why[2], why[3], blocked[0], blocked[1], roadsAreRoutes ? "the route list" : "not the route list: blocked", n0, n, added[0], added[1], added[2],
        (double)(t1.QuadPart - t0.QuadPart) / (double)hz.QuadPart);
    for (int r = 0; r < 4; r++)
        Note("difficulty: stamps: ring %d sides (m) <4 %u, 4-8 %u, 8-16 %u, 16-32 %u, 32-64 %u, 64-128 %u, 128-256 %u, >=256 %u", r,
            sides[r][0], sides[r][1], sides[r][2], sides[r][3], sides[r][4], sides[r][5], sides[r][6], sides[r][7]);
    /* where the level's locations are from the drop point (loc+0 in generator metres, 0..world): the level's own
     * (index < n0) and the added ones, and the added ones' footprint side (sqrt of the box area) by distance */
    {
        static const float edge[8] = {50, 100, 200, 400, 600, 1000, 2000, 4000};
        uint32_t own[9] = {0}, mine[9] = {0}; double area[9] = {0};
        const float half = 8192.0f;
        for (uint32_t i = 0; i < n; i++)
        {
            float xy[2], box[4];
            if (SafeRead(gen + 0x11A7280u + 0xE0ull * i, xy, 8) || SafeRead(gen + 0x11A7280u + 0xE0ull * i + 0xB0, box, 16)) continue;
            float dx = xy[0] - half - g_stamps.centre[0], dy = xy[1] - half - g_stamps.centre[1], d = sqrtf(dx * dx + dy * dy);
            int b = 0; while (b < 8 && d >= edge[b]) b++;
            if (i < n0) own[b]++; else { mine[b]++; area[b] += fabs((double)(box[2] - box[0]) * (double)(box[3] - box[1])); }
        }
        char text[640]; int len = 0;
        for (int b = 0; b < 9; b++)
            len += snprintf(text + len, sizeof text - len, "%s%s%.0f m: %u own, %u added (mean side %.0f m)", b ? "; " : "", b < 8 ? "<" : ">=",
                (double)edge[b < 8 ? b : 7], own[b], mine[b], mine[b] ? sqrt(area[b] / mine[b]) : 0.0);
        Note("difficulty: stamps: locations by distance from the drop point: %s", text);
    }
}

/* At the level's CleanupTerrains (0x65): 1 while the level waits for the tile. */
/* The tile's centre, on the hidden square: the squad's drop point on the war board (difficulty_stream.c: board
 * [game+0x347CEE8], a u8 at +0x17A2B0 = 1 when set, world metres at +0x17A2B4 / +0x17A2B8; chosen before the level
 * generates), or the stamp rings' centre (TerrainStamps, record 91), rounded to the 1024 m grid of the coarse terrain's
 * level-4 nodes. Without either and with `picker`, the dry-ground picker's point (research runs; it also writes
 * xl-drop.txt; it needs the level's height capture). The coarse quadtree's root is 4096 d_c wide and centred at
 * (+d_c/2, +d_c/2) (exe 0x2F9AB3..0x2F9B37: size x N / (N - 1), centre size / 2N); the VS clamps only at +S/2. So its
 * nodes run from x = -8191.75 and from y = -8191.75 upward, in both axes, multiples of 256 samples: the hidden square is
 * [-8191.75 + k d_c, + 512 d_c] in x and in y alike (sample rows count from +y: rows 4095 - (kb + 512) .. 4095 - kb). */
static int NearChooseCentre(float out[2], int picker)
{
    float c[2];
    if (g_stamps.level == g_world.levels) { c[0] = g_stamps.centre[0]; c[1] = g_stamps.centre[1]; }
    else
    {
        uint64_t board = 0; uint8_t flag = 0; float at[2] = {0, 0};
        if (!SafeRead(g_world.game + 0x347CEE8u, &board, 8) && board != 0 && !SafeRead(board + 0x17A2B0u, &flag, 1) && flag == 1 &&
            !SafeRead(board + 0x17A2B4u, &at[0], 4) && !SafeRead(board + 0x17A2B8u, &at[1], 4) && isfinite(at[0]) && isfinite(at[1]) &&
            fabsf(at[0]) <= 7000.0f && fabsf(at[1]) <= 7000.0f)
        { c[0] = at[0]; c[1] = at[1]; }
        else if (picker)
        {
            g_near.ready = 0;
            TerrainNearDrop();
            if (!g_near.ready) return 0;
            c[0] = g_near.centre[0]; c[1] = g_near.centre[1];
        }
        else return 0;
    }
    int kx = (int)floorf((c[0] - 1024.0f + 8192.0f) / (256.0f * 4.0f) + 0.5f) * 256;
    int kb = (int)floorf((c[1] - 1024.0f + 8192.0f) / (256.0f * 4.0f) + 0.5f) * 256;
    if (kx < 256) kx = 256; if (kx > 4096 - 768) kx = 4096 - 768; if (kb < 256) kb = 256; if (kb > 4096 - 768) kb = 4096 - 768;
    out[0] = -8191.75f + (float)kx * NEAR_DC + NEAR_HIDE / 2;
    out[1] = -8191.75f + (float)kb * NEAR_DC + NEAR_HIDE / 2;
    Note("difficulty: near terrain: the hidden square is the coarse terrain's nodes at columns %d..%d, rows %d..%d (from %.0f, %.0f): centre (%.2f, %.2f), "
        "%.3f m sample to sample; the tile %.0f m around it", kx, kx + 512, 4095 - (kb + 512), 4095 - kb, (double)c[0], (double)c[1], (double)out[0],
        (double)out[1], (double)NEAR_HIDE, (double)NEAR_SIDE);
    return 1;
}

static int TerrainNearReplay(uint64_t records, uint32_t end, uint32_t at, uint64_t context, uint64_t g, uint64_t h)
{
    if (g_near.level != g_world.levels)
    {
        MoveLevelGone();
        g_near.level = g_world.levels; g_near.state = NEAR_IDLE; g_near.ready = 0; g_near.handle = 0;
        /* the previous level's tile textures: its world is gone */
        NearReleaseFn release = (NearReleaseFn)NearApi(0x10, 0x198);
        for (int k = 0; k < 2; k++) if (g_near.tex[k] != 0 && release != NULL) release(g_near.tex[k]);
        g_near.tex[0] = g_near.tex[1] = 0;
        NearMatDrop();
        NearCoarseMatDrop();
        if (!NearOn()) { Note("difficulty: near terrain: off (HELLFORGE_XL_NEAR=0)"); return 0; }
        uintptr_t gen = 0, lg = 0, settings = 0, data = 0;
        uint32_t dims[2] = {0};
        if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0 || (lg = gen + 0x8758A8u, 0) || SafeRead(context + 8, &settings, 8) ||
            settings == 0 || SafeRead(lg + 0xC0, dims, 8) || SafeRead(lg + 0xD0, &data, 8) || data == 0 || dims[0] != 4096 || dims[1] != 4096)
        { Note("difficulty: near terrain: the level's capture is not the reviewed one (%u x %u); no tile", dims[0], dims[1]); return 0; }
        size_t bytes = (size_t)dims[0] * dims[1] * 4;
        if (g_near.global == NULL) g_near.global = (float*)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (g_near.global == NULL || SafeRead(data, g_near.global, bytes)) { Note("difficulty: near terrain: the level's heightmap did not copy; no tile"); return 0; }
        g_near.columns = dims[0]; g_near.rows = dims[1];
        NearRingRestore("the level's height capture", NearFetchF32, NearStoreF32, g_near.global);
        if (g_ring.level == g_world.levels) /* the centre the level's ring was drawn around (NearRingStep, at its 0x64) */
        {
            g_near.centre[0] = g_ring.centre[0]; g_near.centre[1] = g_ring.centre[1]; g_near.ready = 1;
        }
        else if (NearChooseCentre(g_near.centre, 1)) g_near.ready = 1;
        if (!g_near.ready) return 0;
        uint32_t size = 0, tiles = 0;
        g_near.lg = lg; g_near.settings = settings;
        if (SafeRead(gen, &g_near.R, 8) || g_near.R == 0 || SafeRead(g_near.R + 0x110, &g_near.gui, 8) || SafeRead(g_near.R, &g_near.cam[0], 8) ||
            SafeRead(g_near.R + 8, &g_near.node[0], 4) || SafeRead(g_near.R + 0x10, &g_near.cam[1], 8) || SafeRead(g_near.R + 0x18, &g_near.node[1], 4) ||
            SafeRead(context + 0x40, &g_near.world, 8) || SafeRead(settings + 0x2C, &size, 4) || SafeRead(lg + 0x120, &g_near.samples, 4) ||
            SafeRead(lg + 0x124, &tiles, 4) || g_near.cam[0] == 0 || g_near.cam[1] == 0 || g_near.gui == 0 || g_near.world == 0)
        { g_near.R = 0; Note("difficulty: near terrain: the merge is not there; no tile"); return 0; }
        if (size != 16384u || tiles != 1 || !(g_near.samples >= 1024.0f && g_near.samples <= 8192.0f))
        { g_near.R = 0; Note("difficulty: near terrain: the merge is not the reviewed one (size %u, %u tiles, %.0f samples); no tile", size, tiles, (double)g_near.samples); return 0; }
        if (g_near.capLevel != g_world.levels) { g_near.R = 0; Note("difficulty: near terrain: the level's captures were not kept; no tile"); return 0; }
        uintptr_t camSlot = NearApi(0x20, 0x20);
        if (camSlot != (uintptr_t)WorldCamSetPosition || !NearRunInstall() || !NearApi(0x20, 0x98) || !NearApi(0x08, 0x100) || !NearApi(0xD0, 0x258) ||
            !NearApi(0xD0, 0x270))
        { g_near.R = 0; Note("difficulty: near terrain: the camera or render interface is not in place; no tile"); return 0; }
        g_near.world_size = (float)size;
        g_near.scale = g_near.world_size / NEAR_SIDE;
        for (int k = 0; k < 2; k++) g_near.offset[k] = -(g_near.centre[k] + g_near.world_size / 2 - NEAR_SIDE / 2) * g_near.samples / NEAR_SIDE;
        if (!NearPlan(records, at)) { g_near.R = 0; Note("difficulty: near terrain: the level's merge records are not all there; no tile"); return 0; }
        g_near.records = records; g_near.end = end; g_near.at65 = at;
        g_near.step = 0; g_near.calls = 0; g_near.building = 0; g_near.tilesDrawn = 0; g_near.tilesFailed = 0; g_near.waits = 0; g_near.frames = 0;
        g_near.started = GetTickCount64();
        g_near.state = NEAR_START;
    }
    if (g_near.state == NEAR_START)
    {
        if (NearQueued(context) != 0) { if (++g_near.waits < 600) return 1; Note("difficulty: near terrain: the job queue did not empty; no tile"); g_near.state = NEAR_FAILED; return 0; }
        NearFindFn find = (NearFindFn)NearApi(0x10, 0x158);
        NearCreateFn create = (NearCreateFn)NearApi(0x10, 0x170);
        NearPublishFn publish = (NearPublishFn)NearApi(0x10, 0x210);
        if (find == NULL || create == NULL || publish == NULL) { g_near.state = NEAR_FAILED; return 0; }
        for (int k = 0; k < 2; k++)
        {
            uint64_t config = find(kNearNames[k]);
            uint32_t format = 0, w = 0, hgt = 0, mips = 0; uint8_t f60 = 0, f63 = 0, f64 = 0;
            if (config == 0 || SafeRead(config + 0x8, &format, 4) || SafeRead(config + 0x14, &w, 4) || SafeRead(config + 0x18, &hgt, 4) ||
                SafeRead(config + 0x20, &mips, 4) || SafeRead(config + 0x60, &f60, 1) || SafeRead(config + 0x63, &f63, 1) || SafeRead(config + 0x64, &f64, 1) ||
                w != 4096 || hgt != 4096)
            { Note("difficulty: near terrain: the render config's 0x%08X is not the reviewed texture (%u x %u); no tile", kNearNames[k], w, hgt); NearReplayEnd(0); return 0; }
            uint32_t flags = (f63 == 1 ? 1u : 0u) | (f60 == 1 ? 2u : 0u) | (f60 == 8 ? 4u : 0u) | (f64 == 0x20 ? 8u : 0u);
            g_near.config[k] = config;
            /* the heightmap at one mip (exe 0x31AF50: r8d = mips, 0 would mean a full chain): every height lookup of the
             * terrain shaders (sample_l at lod 0..4) reads mip 0, as the coarse terrain's does (NearOneMip); the
             * finalize's generate_mips passes 1..4 skip a mip the texture does not have (exe 0x397859) */
            if (k == 0) mips = 1;
            g_near.tex[k] = create(w, hgt, mips, format, 0x48460000u + (uint32_t)k, flags);
            if (g_near.tex[k] == 0) { Note("difficulty: near terrain: texture %d was not created; no tile", k); NearReplayEnd(0); return 0; }
            Note("difficulty: near terrain: tile texture %d: %u x %u, %u mips, format %u, flags 0x%X (as the config's 0x%08X)", k, w, hgt, mips, format, flags, kNearNames[k]);
        }
        for (int k = 0; k < 2; k++) publish(kNearNames[k], g_near.tex[k]);
        publish(kNearRoadAlias, g_near.tex[0]); /* the alias too: the replay's 0x40 must not write the level's heightmap */
        NearCameras(NEAR_SIDE / 2, NEAR_SIDE / 2, g_near.centre[0], g_near.centre[1]);
        MemoryBarrier();
        InterlockedExchange(&g_nearActive, 1);
        g_near.state = NEAR_STEP;
        Note("difficulty: near terrain: replaying the merge for a %.0f m tile at (%.0f, %.0f): %d steps (gui scale %.1f, offset %.1f, %.1f); the level waits",
            (double)NEAR_SIDE, (double)g_near.centre[0], (double)g_near.centre[1], g_near.steps, (double)g_near.scale, (double)g_near.offset[0], (double)g_near.offset[1]);
    }
    if (g_near.state == NEAR_STEP)
    {
        g_near.frames++;
        if (g_stepDump.phase != 0)
        {
            int d = NearStepDump(records, end, context, g, h);
            if (d < 0) { Note("difficulty: near terrain: a step dump failed; no tile"); g_stepDump.phase = 0; NearReplayEnd(0); return 0; }
            if (d == 0 && g_near.step == g_near.steps) { g_near.state = NEAR_DRAIN; g_near.waits = 0; }
            return 1;
        }
        int r = NearRunStep(records, end, context, g, h);
        if (r < 0)
        {
            Note("difficulty: near terrain: step %d (op 0x%02X) failed; no tile", g_near.step, g_near.stepOp[g_near.step]);
            NearReplayEnd(0);
            return 0;
        }
        if (r > 0 && NearStepDumpAfter(g_near.step))
        {
            g_stepDump.s64 = -1;
            for (int s = 0; s < g_near.steps; s++) if (g_near.stepKind[s] == NEAR_RECORD && g_near.stepOp[s] == 0x64) g_stepDump.s64 = s;
            if (g_stepDump.s64 >= 0) { g_stepDump.back = g_near.step + 1; g_stepDump.phase = 1; g_stepDump.waits = 0; return 1; }
        }
        if (r > 0 && ++g_near.step == g_near.steps) { g_near.state = NEAR_DRAIN; g_near.waits = 0; }
        return 1;
    }
    if (g_near.state == NEAR_DRAIN)
    {
        /* the queue empty, then a few frames for the consumer's last jobs; then the readback (after them on the GPU) */
        if (NearQueued(context) != 0 || ++g_near.waits < 4) { if (g_near.waits < 600) return 1; NearReplayEnd(0); return 0; }
        NearReadbackFn readback = (NearReadbackFn)NearApi(0x10, 0x1A8);
        if (readback == NULL) { NearReplayEnd(0); return 0; }
        g_near.readId = readback(g_near.tex[0]);
        g_near.state = NEAR_READBACK; g_near.polls = 0;
        return 1;
    }
    if (g_near.state == NEAR_CONFORM)
    {
        g_near.frames++;
        if (NearConformStep(records, end, context, g, h)) return 1;
        NearReplayEnd(1);
        g_near.state = NEAR_SPAWN;
        return 0;
    }
    if (g_near.state == NEAR_CAL)
    {
        g_near.frames++;
        if (NearCalStep(records, end, context, g, h)) return 1;
        NearReplayEnd(1);
        Note("difficulty: near terrain: calibration done; the tile goes on as the texture now is");
        g_near.state = NEAR_SPAWN;
        return 0;
    }
    if (g_near.state == NEAR_READBACK)
    {
        NearReadyFn ready = (NearReadyFn)NearApi(0x10, 0x1D0);
        NearDescFn desc = (NearDescFn)NearApi(0x10, 0x1D8);
        if (ready == NULL || desc == NULL) { NearReplayEnd(0); return 0; }
        if (!ready(g_near.readId)) { if (++g_near.polls < 2000) return 1; Note("difficulty: near terrain: the tile's readback did not come; no tile"); NearReplayEnd(0); return 0; }
        uint8_t out[32] = {0}, d[24];
        const uint8_t* p = desc(out, g_near.readId);
        memcpy(d, p ? p : out, 24);
        uint32_t w, hgt, format; uint64_t data;
        memcpy(&w, d, 4); memcpy(&hgt, d + 4, 4); memcpy(&format, d + 8, 4); memcpy(&data, d + 16, 8);
        /* the config's generated_heightmap is format 22 (16 bits a texel: u16, v / 65535 x 1000 - 500 m); a 32-bit format
         * would be a normalized float */
        uint32_t texel = NearTexelBytes(format);
        if (data == 0 || w != 4096 || hgt != 4096 || (texel != 2 && texel != 4))
        { Note("difficulty: near terrain: the tile's readback is %u x %u format %u; no tile", w, hgt, format); NearReplayEnd(0); return 0; }
        size_t count = (size_t)w * hgt;
        if (g_near.heights == NULL) g_near.heights = (float*)VirtualAlloc(NULL, count * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (g_near.heights == NULL || SafeRead(data, g_near.heights, count * texel)) { NearReplayEnd(0); return 0; }
        if (texel == 2) /* widen in place, from the end */
        {
            const uint16_t* u = (const uint16_t*)g_near.heights;
            for (size_t i = count; i-- > 0;) g_near.heights[i] = (float)u[i] / 65535.0f;
        }
        g_near.hw = w; g_near.hh = hgt;
        double sum = 0, diff = 0; float lo = 1e9f, hi = -1e9f; uint32_t zeros = 0, n = 0;
        for (uint32_t r = 0; r < hgt; r += 16)
            for (uint32_t c = 0; c < w; c += 16)
            {
                float v = g_near.heights[(size_t)r * w + c];
                if (v == 0.0f) zeros++;
                float m = v * 1000.0f - 500.0f;
                float wx = g_near.centre[0] - NEAR_SIDE / 2 + (c + 0.5f) * NEAR_SIDE / w, wy = g_near.centre[1] + NEAR_SIDE / 2 - (r + 0.5f) * NEAR_SIDE / hgt;
                diff += fabs((double)m - (double)NearGlobalAt(wx, wy));
                if (m < lo) lo = m; if (m > hi) hi = m; sum += m; n++;
            }
        if (NearConformOn() && g_near.state == NEAR_READBACK && !NearCalOn())
        {
            Note("difficulty: near terrain: the tile is rendered (%.1f s): heights %.1f..%.1f m; mean |tile - level capture| %.2f m; conforming it to the coarse terrain",
                (double)(GetTickCount64() - g_near.started) / 1000.0, (double)lo, (double)hi, n ? diff / n : 0.0);
            { uint16_t* c = g_conform.coarse; float* l = g_conform.low; float* d = g_conform.delta; memset(&g_conform, 0, sizeof g_conform);
              g_conform.coarse = c; g_conform.low = l; g_conform.delta = d; }
            g_near.state = NEAR_CONFORM;
            return 1;
        }
        if (NearCalOn() && g_near.state == NEAR_READBACK)
        {
            Note("difficulty: near terrain: the tile is rendered (%.1f s): heights %.1f..%.1f m; calibrating", (double)(GetTickCount64() - g_near.started) / 1000.0,
                (double)lo, (double)hi);
            { uint16_t* keep = g_cal.buf; memset(&g_cal, 0, sizeof g_cal); g_cal.buf = keep; }
            g_near.state = NEAR_CAL;
            return 1;
        }
        NearReplayEnd(1);
        Note("difficulty: near terrain: the tile is rendered in %.1f s (%u frames; %d vanilla-density tiles drawn, %d failed): heights %.1f..%.1f m, mean %.1f, "
            "%u of %u samples zero; mean |tile - level| %.2f m", (double)(GetTickCount64() - g_near.started) / 1000.0, g_near.frames, g_near.tilesDrawn,
            g_near.tilesFailed, (double)lo, (double)hi, n ? sum / n : 0.0, zeros, n, n ? diff / n : 0.0);
        WorldMemoryNote("near tile rendered", 0xFFFFFFFFu, 0, 1);
        if (zeros * 2 > n) { Note("difficulty: near terrain: the tile is mostly empty; it is not spawned"); NearReplayEnd(0); return 0; }
        g_near.state = NEAR_SPAWN;
        return 0;
    }
    return 0;
}

/* The render config's slots for the two names, as the spawn (exe 0x1AABE0) looks them up: exe 0xA6E30 on
 * [[world+0x2301F8]]+0x1600 (a find-or-insert returning the value's address; both names are render-config resources). 1
 * when both hold the textures the replay saw there and the tile's textures are of the same resource type (byte +3, which
 * the spawn branches on). */
static int NearSpawnSlots(uint64_t world)
{
    uint64_t holder = 0, renderer = 0;
    g_nearSlots[0] = g_nearSlots[1] = NULL;
    if (SafeRead(world + 0x2301F8u, &holder, 8) || holder == 0 || SafeRead(holder, &renderer, 8) || renderer == 0) return 0;
    NearSlotFn slotOf = (NearSlotFn)((uintptr_t)GetModuleHandleW(NULL) + 0xA6E30u);
    uint64_t now[2] = {0, 0};
    uint8_t typeOurs[2] = {0}, typeConfig[2] = {0};
    int fine = 1;
    for (int k = 0; k < 2; k++)
    {
        uint32_t key = kNearNames[k];
        g_nearSlots[k] = slotOf(renderer + 0x1600, &key);
        /* with the level's one-mip copy published, the replay's find saw the copy; the slot keeps the config's own */
        uint64_t seen = k == 0 && g_oneMip.level == g_world.levels && g_oneMip.tex != 0 && g_near.config[0] == g_oneMip.tex ? g_oneMip.config : g_near.config[k];
        if (g_nearSlots[k] == NULL || SafeRead((uintptr_t)g_nearSlots[k], &now[k], 8) || now[k] != seen ||
            SafeRead(g_near.tex[k] + 3, &typeOurs[k], 1) || SafeRead(now[k] + 3, &typeConfig[k], 1) || typeOurs[k] != typeConfig[k]) fine = 0;
    }
    if (!fine)
        Note("difficulty: near terrain: the render config's slots are not the textures seen before (0x%llX 0x%llX vs 0x%llX 0x%llX, types %u/%u %u/%u); "
            "no tile", (unsigned long long)now[0], (unsigned long long)now[1], (unsigned long long)g_near.config[0], (unsigned long long)g_near.config[1],
            typeOurs[0], typeConfig[0], typeOurs[1], typeConfig[1]);
    return fine;
}

/* [api+8]+0x198 while InitSpawnTerrain spawns the tile: its pose, size and, for this call, the textures. */
static uint64_t NearSpawn(void* world, float* pose, uint32_t a, uint32_t type, uint32_t name, uint64_t resource, uint32_t hash, float* size,
    float* range, uint8_t flag)
{
    if (!(type == kNearNames[0] && name == kNearNames[1] && g_nearSpawnThread == GetCurrentThreadId() && g_nearSlots[0] && g_nearSlots[1]))
        return g_nearSpawnOriginal(world, pose, a, type, name, resource, hash, size, range, flag);
    pose[12] += g_near.centre[0]; pose[13] += g_near.centre[1];
    size[0] = NEAR_SPAN; size[1] = NEAR_SPAN; /* sample to sample (texel span x 4095 / 4096) */
    uint64_t keep[2] = {*g_nearSlots[0], *g_nearSlots[1]};
    for (int k = 0; k < 2; k++) *g_nearSlots[k] = g_near.tex[k];
    int band = g_matband.tex != 0 && NearMatRedirect(1);
    if (band) *g_nearSlots[1] = g_matband.tex;
    uint64_t r = g_nearSpawnOriginal(world, pose, a, type, name, resource, hash, size, range, flag);
    if (band) NearMatRedirect(0);
    for (int k = 0; k < 2; k++) *g_nearSlots[k] = keep[k];
    return r;
}

static uint64_t NearOneMipSpawn(void* world, float* pose, uint32_t a, uint32_t type, uint32_t name, uint64_t resource, uint32_t hash, float* size,
    float* range, uint8_t flag)
{
    NearSpawnFn original = (NearSpawnFn)g_oneMip.original;
    if (!(type == kNearNames[0] && name == kNearNames[1] && g_oneMip.armed && g_oneMip.thread == GetCurrentThreadId() && g_oneMip.slot != NULL))
        return original(world, pose, a, type, name, resource, hash, size, range, flag);
    uint64_t keep = *g_oneMip.slot, keepMat = g_coarseMat.slot != NULL ? *g_coarseMat.slot : 0;
    *g_oneMip.slot = g_oneMip.tex;
    int mat = g_coarseMat.slot != NULL && g_coarseMat.tex != 0 && NearMatRedirect(1);
    if (mat) *g_coarseMat.slot = g_coarseMat.tex;
    uint64_t r = original(world, pose, a, type, name, resource, hash, size, range, flag);
    if (mat) { *g_coarseMat.slot = keepMat; NearMatRedirect(0); }
    *g_oneMip.slot = keep;
    if (!g_oneMip.spawned++)
        Note("difficulty: near terrain: the level's terrain spawned with the one-mip heightmap%s", !mat ? "" : g_coarseMat.served == 1 ?
            " and the interpolated material map (its physics materials from its pixels)" : g_coarseMat.served == 2 ?
            " and the interpolated material map (its physics materials read back from the level's map)" : " and the interpolated material map (no physics readback asked)");
    return r;
}

/* The coarse terrain under the tile, once the tile is in (the level terrain h = [G+0x8758A0], W x H samples from
 * [[h+0x58]+0x14/+0x18], 4 m each on 16 km; sample column i at x = 4 i - 8192, row j at y = 8192 - 4 j):
 * - its collision goes to the lowest height: [api+8]+0x1D0 (exe 0x3369E0 -> 0x1ADCD0: world, h, x0, y0, x1, y1
 *   exclusive, rows from the +y edge, u16 heights v / 65535 x 1000 - 500 as a w-wide rectangle from (x0, y0), materials
 *   (may be NULL), w, rows), the samples strictly inside the tile only, so the 4 m ramp up to the coarse ground at each
 *   border lies under the tile's own collision;
 * - its drawing: the same edit copies the height-bounds quadtree to [[h+0x40]+8] ({f32 min, f32 max, u32 child[4]}, root
 *   0, child -1 = leaf; children 0 low column/low B, 1 high/low, 2 high/high, 3 low/high, split at the midpoints; B = H
 *   - row), which the render adopts as it is; its cull (exe 0x2F3AA0 -> 0x2F34F0) drops a node whose box is NaN in
 *   every view. Every node wholly inside the tile gets NaN bounds (the tile is whole 256 m nodes: 64 samples).
 * A later edit inside the tile (a crater on the coarse terrain) un-hides what it touches (min/max grow with minss/maxss). */
/* What NearHideCoarse hid (the nodes and their bounds) and the tile's root node, for the test eye's A/B. */
#define NEAR_HIDDEN_MAX 4096
#define NEAR_TILES_MAX 8
static struct { LONG level; int coarseCount; uint8_t* coarseNode[NEAR_HIDDEN_MAX]; float coarseBounds[NEAR_HIDDEN_MAX][2];
    uint8_t* tileRoot; float tileBounds[2]; int coarseShown; uintptr_t nodes; uint32_t count; } g_nearShown;
/* What NearHideCoarse did for each live tile (g_tiles' slots): the coarse samples [x0, x0 + n] x [y0, y0 + n] (column,
 * row from the +y edge; the inside lowered, the border kept), the nodes it hid and their bounds before. */
typedef struct { LONG level; int x0, y0, n, lowered, count; uint8_t* node[NEAR_HIDDEN_MAX]; float bounds[NEAR_HIDDEN_MAX][2]; uintptr_t nodes; uint32_t total; } NearHidden;
static NearHidden g_tileHidden[NEAR_TILES_MAX];

/* The coarse square of a tile: its samples [x0, x0 + 512] x [y0, y0 + 512] (column; row from the +y edge). On the grid of
 * the ring (NearRingStep) when the ring was drawn: the drop tile's square is the ring's, a cell (a, b) of the grid is
 * 512 samples further per step (b north); otherwise from the centre, as before the grid. */
static void NearCellSquare(int a, int b, float cx, float cy, int* x0, int* y0)
{
    if (g_ring.level == g_world.levels && g_ring.applied) { *x0 = g_ring.c0 + 512 * a; *y0 = g_ring.r0 - 512 * b; return; }
    *x0 = (int)((cx - NEAR_HIDE / 2 + 8192.0f) / 4.0f + 0.5f);
    *y0 = (int)((8192.0f - (cy + NEAR_HIDE / 2)) / 4.0f + 0.5f);
}

/* The level terrain's handle and its 4096 x 4096 dims; 0 when it is not the reviewed one. */
static uint64_t NearCoarseHandle(void)
{
    uintptr_t gen = 0, h = 0, dimsAt = 0;
    uint32_t W = 0, H = 0;
    if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0 || SafeRead(gen + 0x8758A0u, &h, 8) || h == 0 || SafeRead(h + 0x58, &dimsAt, 8) ||
        dimsAt == 0 || SafeRead(dimsAt + 0x14, &W, 4) || SafeRead(dimsAt + 0x18, &H, 4) || W != 4096 || H != 4096) return 0;
    return h;
}

/* The coarse collision under a tile's square goes to the lowest height ([api+8]+0x1D0): the samples strictly inside. */
static void NearHideCoarse(uint64_t world, int x0, int y0, NearHidden* out)
{
    out->level = g_world.levels; out->count = 0; out->nodes = 0; out->total = 0; out->lowered = 0; out->x0 = x0; out->y0 = y0; out->n = 512;
    uint64_t h = NearCoarseHandle();
    if (h == 0) { Note("difficulty: near terrain: the level terrain is not the reviewed one; the coarse ground stays under the tile"); return; }
    const int n = 512, W = 4096, H = 4096;
    if (x0 < 1 || y0 < 1 || x0 + n >= W || y0 + n >= H) { Note("difficulty: near terrain: the tile is not inside the level terrain"); return; }
    typedef void (*EditFn)(uint64_t world, uint64_t h, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const uint16_t* heights, const uint32_t* materials,
        uint32_t w, uint32_t rows);
    EditFn edit = (EditFn)NearApi(0x08, 0x1D0);
    if (edit == NULL) return;
    uint32_t w = (uint32_t)(n - 1);
    uint16_t* low = (uint16_t*)VirtualAlloc(NULL, (size_t)w * w * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); /* zeros: -500 m */
    if (low == NULL) return;
    edit(world, h, x0 + 1, y0 + 1, x0 + n, y0 + n, low, NULL, w, w);
    VirtualFree(low, 0, MEM_RELEASE);
    out->lowered = 1;
    Note("difficulty: near terrain: the coarse terrain's collision under the tile lowered (samples %d..%d, rows %d..%d)", x0 + 1, x0 + n - 1, y0 + 1, y0 + n - 1);
}

/* The coarse collision under a released tile's square back as the level made it: the rows of the level's CPU copy
 * (lg+0xD8: {w, h, format, ?, data}, u16, the heights the coarse terrain was spawned from, with the craters the
 * deformation copied in since). 1 when done. */
static int NearRaiseCoarse(uint64_t world, const NearHidden* sq)
{
    uint64_t h = NearCoarseHandle();
    uintptr_t gen = 0; uint32_t d[3] = {0}; uint64_t data = 0;
    typedef void (*EditFn)(uint64_t world, uint64_t h, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const uint16_t* heights, const uint32_t* materials,
        uint32_t w, uint32_t rows);
    EditFn edit = (EditFn)NearApi(0x08, 0x1D0);
    if (h == 0 || edit == NULL || !sq->lowered || SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0 || SafeRead(gen + 0x8758A8u + 0xD8, d, 12) ||
        SafeRead(gen + 0x8758A8u + 0xD8 + 0x10, &data, 8) || data == 0 || d[0] != 4096 || d[1] != 4096 || NearTexelBytes(d[2]) != 2) return 0;
    uint32_t w = (uint32_t)(sq->n - 1);
    uint16_t* rows = (uint16_t*)VirtualAlloc(NULL, (size_t)w * w * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (rows == NULL) return 0;
    int ok = 1;
    for (uint32_t r = 0; r < w && ok; r++)
        ok = !SafeRead(data + ((uint64_t)(sq->y0 + 1 + r) * 4096 + (uint64_t)(sq->x0 + 1)) * 2, rows + (size_t)r * w, (size_t)w * 2);
    if (ok) edit(world, h, sq->x0 + 1, sq->y0 + 1, sq->x0 + sq->n, sq->y0 + sq->n, rows, NULL, w, w);
    VirtualFree(rows, 0, MEM_RELEASE);
    return ok;
}

/* ---- A spawned terrain's own heights (the seam report, the dump, the eye's ground) ------------------------------------
 * Each terrain object keeps a u16 copy of its heights: obj = [entry+0x10] (entry: exe 0x1B86C0 on world+0x230550, a
 * find-or-insert keyed by the handle), heights [obj+0x40], metres v / 65535 x 1000 - 500; sample (i, j) of N at
 * terrain-local x = -size/2 + i size/(N-1), y = +size/2 - j size/(N-1) (row 0 the +y edge; exe 0x2F2CB0's mapping,
 * confirmed in game by run i16). N from [[h+0x58]+0x14/+0x18]. */
typedef struct { const uint16_t* v; uint32_t n; float size, cx, cy; } NearGround;

static int NearGroundOf(uint64_t world, uint64_t handle, float size, float cx, float cy, NearGround* out, uint16_t** keep)
{
    typedef uint8_t* (*FindFn)(uint64_t table, uint64_t* key);
    uint64_t key = handle, obj = 0, data = 0, dimsAt = 0;
    uint32_t w = 0, hgt = 0;
    memset(out, 0, sizeof *out);
    if (handle == 0 || SafeRead(handle + 0x58, &dimsAt, 8) || dimsAt == 0 || SafeRead(dimsAt + 0x14, &w, 4) || SafeRead(dimsAt + 0x18, &hgt, 4) ||
        w != hgt || w < 2 || w > 8192) return 0;
    uint8_t* e = ((FindFn)((uintptr_t)GetModuleHandleW(NULL) + 0x1B86C0u))(world + 0x230550u, &key);
    if (e == NULL || SafeRead((uintptr_t)e + 0x10, &obj, 8) || obj == 0 || SafeRead(obj + 0x40, &data, 8) || data == 0) return 0;
    uint16_t probe;
    if (SafeRead(data + ((uint64_t)w * w - 1) * 2, &probe, 2)) return 0;
    /* the object's copy does not stay (its memory is reused later on): ours, taken now (the caller's buffer: one for the
     * coarse terrain, one per tile slot) */
    if (*keep == NULL) *keep = (uint16_t*)VirtualAlloc(NULL, (size_t)4096 * 4096 * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (*keep == NULL || w != 4096 || SafeRead(data, *keep, (size_t)w * w * 2)) return 0;
    out->v = *keep; out->n = w; out->size = size; out->cx = cx; out->cy = cy;
    return 1;
}

/* Bilinear between the four samples around a world point, as the terrain's triangles approximate it (-9999 outside). */
static float NearGroundAt(const NearGround* t, float wx, float wy)
{
    if (t->v == NULL) return -9999.0f;
    float step = t->size / (float)(t->n - 1);
    float fx = (wx - t->cx + t->size / 2) / step, fy = (t->cy + t->size / 2 - wy) / step;
    if (fx < 0 || fy < 0 || fx > (float)(t->n - 1) || fy > (float)(t->n - 1)) return -9999.0f;
    uint32_t c = (uint32_t)fx, r = (uint32_t)fy;
    if (c >= t->n - 1) c = t->n - 2;
    if (r >= t->n - 1) r = t->n - 2;
    float ax = fx - c, ay = fy - r;
    const uint16_t* row0 = t->v + (size_t)r * t->n, *row1 = row0 + t->n;
    float top = row0[c] * (1 - ax) + row0[c + 1] * ax, bottom = row1[c] * (1 - ax) + row1[c + 1] * ax;
    return (top * (1 - ay) + bottom * ay) / 65535.0f * 1000.0f - 500.0f;
}

static NearGround g_nearCoarseGround, g_nearTileGround;

/* ---- The detailed tiles in the world ----------------------------------------------------------------------------------
 * Every spawned detailed tile (the drop tile first; more follow the players, NearMove): its centre (its hidden coarse
 * square is NEAR_HIDE around it, its terrain NEAR_SPAN), its terrain handle and its CPU heights (the spawned terrain's
 * own copy, NearGroundOf). The ground queries (NearInHidden, NearVisibleAt) and the scatter go over the live ones;
 * g_tilesGen counts changes (the scatter range cache starts over). Written on the game thread; read by render and
 * worker threads (scatter, layers) as a snapshot under g_tilesLock. */
/* cell: the tile's cell of the grid (the drop tile's is 0, 0); tex/band/bandPixels: its own maps (a tile built in
 * mission: released with it; the drop tile's stay g_near's and g_matband's); near: the last time a player was within
 * reach (difficulty_world_move.c). */
typedef struct { int live; LONG level; float centre[2]; uint64_t handle; NearGround ground; int cell[2]; uint64_t tex[2], band; uint32_t* bandPixels;
    ULONGLONG seen; } NearTile;
static uint16_t* g_groundBuf[NEAR_TILES_MAX + 1]; /* NearGroundOf's copies: a tile slot each, the coarse terrain last */
static NearTile g_tiles[NEAR_TILES_MAX];
static volatile LONG g_tilesGen;
static SRWLOCK g_tilesLock = SRWLOCK_INIT;

static int NearTilesLive(void)
{
    int n = 0;
    for (int k = 0; k < NEAR_TILES_MAX; k++) n += g_tiles[k].live && g_tiles[k].level == g_world.levels;
    return n;
}

/* A free slot (not live for this level), -1 when all are taken. Game thread only (the only writer). */
static int NearTileFree(void)
{
    for (int k = 0; k < NEAR_TILES_MAX; k++) if (!g_tiles[k].live || g_tiles[k].level != g_world.levels) return k;
    return -1;
}

static int NearTileAdd(int at, float cx, float cy, uint64_t handle, const NearGround* ground)
{
    if (at < 0 || at >= NEAR_TILES_MAX) return -1;
    AcquireSRWLockExclusive(&g_tilesLock);
    g_tiles[at].live = 1; g_tiles[at].level = g_world.levels; g_tiles[at].centre[0] = cx; g_tiles[at].centre[1] = cy;
    g_tiles[at].handle = handle; g_tiles[at].ground = *ground; g_tiles[at].seen = GetTickCount64();
    InterlockedIncrement(&g_tilesGen);
    ReleaseSRWLockExclusive(&g_tilesLock);
    return at;
}

static void NearTileRemove(int at)
{
    AcquireSRWLockExclusive(&g_tilesLock);
    if (at >= 0 && at < NEAR_TILES_MAX) { g_tiles[at].live = 0; g_tiles[at].ground.v = NULL; InterlockedIncrement(&g_tilesGen); }
    ReleaseSRWLockExclusive(&g_tilesLock);
}

static void NearTilesClear(void)
{
    AcquireSRWLockExclusive(&g_tilesLock);
    for (int k = 0; k < NEAR_TILES_MAX; k++) { g_tiles[k].live = 0; g_tiles[k].ground.v = NULL; }
    InterlockedIncrement(&g_tilesGen);
    ReleaseSRWLockExclusive(&g_tilesLock);
}

/* The coarse terrain's drawing under the live tiles: the edit copies the height-bounds quadtree to [[h+0x40]+8]
 * ({f32 min, f32 max, u32 child[4]}, root 0, child -1 = leaf; children 0 low column/low B, 1 high/low, 2 high/high, 3
 * low/high, split at the midpoints; B = H - 1 - row), which the render adopts as it is; its cull (exe 0x2F3AA0 ->
 * 0x2F34F0, the box's 8 corners in clip space, one inside every plane) drops a node whose box is NaN in every view (an
 * inverted box would not do: its corners still span both z extremes). A node goes NaN when the live tiles' squares cover
 * it whole: one square, or on the ring's grid every cell it touches (adjacent tiles: the nodes across their shared
 * border are in neither square alone); a node no longer covered (a tile released) gets the bounds it had back. The whole
 * tree is walked on every change (some 5500 nodes).
 * Every height edit ([api+8]+0x1D0 -> exe 0x1ADCD0, under the lock at world+0x230520 -> 0x2F63D0 -> 0x2F5370) sets the
 * leaves it touches with minss/maxss against the old bounds (a NaN leaf gets the new values: un-hidden) and then each
 * node on its path to its children's union, as maxss(maxss(maxss(maxss(-FLT_MAX, c0), c1), c2), c3) (0x2F5867..
 * 0x2F592E): a NaN child 3 makes the parent NaN, and the parent's parent when it is a child 3 too; a NaN child 0..2 drops
 * the children before it. Run s72l: cell (1, 0)'s lowering (columns 2305..2815) went through three nodes whose child 3
 * was one of the drop tile's hidden nodes and NaN'd the root's child 1, the coarse terrain's whole south-east quarter
 * (x > 0, y < 0), seen from the air as the sea plane where the coarse terrain should be. So after every edit of the
 * coarse terrain (the loader's and the game's craters: NearBoundsDetour on 0x2F63D0, whose only caller is 0x1ADCD0, in
 * the lock) NearBoundsFix puts the tree right: the hidden nodes NaN, every other node its children's union, a hidden
 * node counted at its own bounds (the coarse heights there, as the far LODs draw its ancestors whole). */
static struct { LONG level; uintptr_t nodes; uint32_t total; uint8_t* hidden; float* saved; int count; uint64_t handle; volatile LONG64 fixes, fixed; } g_hide = {.level = -1};
typedef uint64_t (*NearBoundsFn)(uint64_t h, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9,
    uint64_t a10, uint64_t a11);
static NearBoundsFn g_nearBoundsOriginal;
typedef void (*NearLockFn)(uint64_t lock);
static NearLockFn g_nearTerrainLock[2]; /* the game's own: [exe+0x13F66B8] (0x1ADD06), [exe+0x13F66C0] (0x1ADE63) */

/* Node k's subtree put right; out = its bounds as drawn when not hidden (a hidden node: its own). 0 when none are finite. */
static int NearBoundsFix(uintptr_t nodes, uint32_t count, uint32_t k, int depth, float out[2], int* changed)
{
    if (k >= count || depth > 24) return 0;
    uint8_t* node = (uint8_t*)(nodes + (uint64_t)k * 24);
    uint32_t child[4]; float cur[2], box[2] = {0, 0};
    memcpy(child, node + 8, 16); memcpy(cur, node, 8);
    int have = 0;
    if (child[0] == 0xFFFFFFFFu) { if (!isnan(cur[0]) && !isnan(cur[1])) { box[0] = cur[0]; box[1] = cur[1]; have = 1; } }
    else
        for (int q = 0; q < 4; q++)
        {
            float b[2];
            if (!NearBoundsFix(nodes, count, child[q], depth + 1, b, changed)) continue;
            if (!have) { box[0] = b[0]; box[1] = b[1]; have = 1; }
            else { box[0] = fminf(box[0], b[0]); box[1] = fmaxf(box[1], b[1]); }
        }
    if (g_hide.hidden[k])
    {
        float* s = g_hide.saved + 2 * (size_t)k;
        if (have && child[0] != 0xFFFFFFFFu) { s[0] = box[0]; s[1] = box[1]; } /* its children's union: the coarse heights now */
        else if (have) { s[0] = fminf(s[0], box[0]); s[1] = fmaxf(s[1], box[1]); } /* a leaf an edit un-hid: what it had and the edit's */
        if (!isnan(cur[0]) || !isnan(cur[1])) { *(volatile uint64_t*)node = 0x7FC000007FC00000ull; (*changed)++; }
        if (isnan(s[0]) || isnan(s[1])) return 0;
        out[0] = s[0]; out[1] = s[1];
        return 1;
    }
    if (!have) return 0;
    if (child[0] != 0xFFFFFFFFu && !(cur[0] == box[0] && cur[1] == box[1]))
    {
        uint64_t v; memcpy(&v, box, 8);
        *(volatile uint64_t*)node = v; (*changed)++;
    }
    out[0] = box[0]; out[1] = box[1];
    return 1;
}

/* The whole tree, when it is still the array NearHideUnion hid nodes in. Under the terrain lock. Returns the nodes written. */
static int NearBoundsFixAll(uint64_t h)
{
    uintptr_t cow = 0, nodes = 0; uint32_t count = 0; float b[2]; int changed = 0;
    if (g_hide.level != g_world.levels || g_hide.hidden == NULL || h == 0 || h != g_hide.handle || SafeRead(h + 0x40, &cow, 8) || cow == 0 ||
        SafeRead(cow, &count, 4) || SafeRead(cow + 8, &nodes, 8) || nodes != g_hide.nodes || count != g_hide.total) return -1;
    NearBoundsFix(nodes, count, 0, 0, b, &changed);
    return changed;
}

/* After the game's bounds update of any terrain: the coarse terrain's tree put right when it hides nodes. */
static uint64_t NearBoundsDetour(uint64_t h, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9,
    uint64_t a10, uint64_t a11)
{
    uint64_t r = g_nearBoundsOriginal(h, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
    if (h != 0 && h == g_hide.handle && g_hide.level == g_world.levels && g_hide.count > 0)
    {
        int n = NearBoundsFixAll(h);
        if (n >= 0) { InterlockedIncrement64(&g_hide.fixes); if (n > 0) InterlockedExchangeAdd64(&g_hide.fixed, n); }
    }
    return r;
}

static void NearBoundsInstall(void)
{
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    if (g_nearBoundsOriginal != NULL) return;
    if (!ApiCodeMatches(exe + 0x2F63D0u, "48 8B C4 55 41 54 48 8D 68 E8 48 81 EC 28 01 00 00") ||
        !ApiCodeMatches(exe + 0x1ADCF6u, "4C 8D A9 20 05 23 00 48 8B D9 49 8B CD 41 8B F1 FF 15 AC 89 24 01") ||
        !ApiCodeMatches(exe + 0x1ADE41u, "E8 8A 85 14 00 49 8B CD") || !ApiCodeMatches(exe + 0x1ADE63u, "48 FF 25 56 88 24 01"))
    { Note("difficulty: near terrain: the terrain bounds update is not the reviewed code; an edit next to a hidden node can hide more of the coarse terrain"); return; }
    if (SafeRead(exe + 0x13F66B8u, &g_nearTerrainLock[0], 8) || SafeRead(exe + 0x13F66C0u, &g_nearTerrainLock[1], 8) || g_nearTerrainLock[0] == NULL ||
        g_nearTerrainLock[1] == NULL || LoaderHook("extra large maps' coarse bounds repair", exe + 0x2F63D0u, (void*)NearBoundsDetour, (void**)&g_nearBoundsOriginal))
    { g_nearTerrainLock[0] = g_nearTerrainLock[1] = NULL; Note("difficulty: near terrain: the terrain bounds update could not be hooked; an edit next to a hidden node can hide more of the coarse terrain"); }
}

/* The physics scene's command stream (scene [exe+0x190D4A0 + w x 0x80], stream [scene+0x4A0]: +0x10 the block being
 * written, +0x18 bytes used in it). In async mode the game's own writers append under the world's lock
 * [exe+0x27C6350 + w x 8] (Scene::remove_body 0xBE8D10 from 0x812AE0, add_body 0xBE8840, body creation 0x812CC0), while
 * the world's physics job swaps the stream at the start of its step (0xC9513A -> 0xBB1430 -> 0xAD44B0: +0x10 = 0 at
 * 0xAD4505, +0x18 left as it was, then a new pool and 0xAD48E0) with no lock. A writer between the two read block 0
 * and used 0x100 and stored at 0x120 (run s73a, exe+0xBE8E06 on the world update, 0.4 s after an extra large level's
 * mission content spawned at load; vanilla has the same window, it is just narrow). On extra large levels the swap
 * runs under the same lock, a recursive critical section (the imports at exe+0x13F66B8/+0x13F66C0, as the terrain
 * lock): no code the locked writers call reaches the job's wait 0x7D7930 by direct calls (call graph from 0x812CC0,
 * 0x812510, 0xBE8D10, 0xBE8840 and their callees; their indirect calls not followed). */
typedef uint64_t (*SceneSwapFn)(uint64_t scene);
static SceneSwapFn g_sceneSwapOriginal;
static uintptr_t g_sceneSwapExe;
static volatile LONG64 g_sceneSwapsLocked;
static uint64_t SceneSwapDetour(uint64_t scene)
{
    if (!(g_world.current > 1.0f) || g_nearTerrainLock[0] == NULL || g_nearTerrainLock[1] == NULL) return g_sceneSwapOriginal(scene);
    uint64_t lock = 0;
    for (uintptr_t w = 0; w < 4 && lock == 0; w++)
        if (*(volatile uint64_t*)(g_sceneSwapExe + 0x190D4A0u + w * 0x80u) == scene) lock = *(volatile uint64_t*)(g_sceneSwapExe + 0x27C6350u + w * 8u);
    if (lock == 0) return g_sceneSwapOriginal(scene);
    g_nearTerrainLock[0](lock);
    uint64_t result = g_sceneSwapOriginal(scene);
    g_nearTerrainLock[1](lock);
    InterlockedIncrement64(&g_sceneSwapsLocked);
    return result;
}

static void SceneSwapInstall(void)
{
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    if (g_sceneSwapOriginal != NULL) return;
    if (g_nearTerrainLock[0] == NULL || g_nearTerrainLock[1] == NULL) { Note("difficulty: extra large maps: the physics stream swap stays unlocked (the terrain lock's calls did not read)"); return; }
    if (!ApiCodeMatches(exe + 0xBB1430u, "40 53 48 83 EC 20 48 8B D9 48 8B 89 A0 04 00 00") || !ApiCodeMatches(exe + 0xC9513Au, "E8 F1 C2 F1 FF") ||
        !ApiCodeMatches(exe + 0xAD4505u, "4C 89 43 10 4C 89 03") || !ApiCodeMatches(exe + 0x812C24u, "48 8B AC 2F 50 63 7C 02"))
    { Note("difficulty: extra large maps: the physics stream swap is not the reviewed code; it stays unlocked"); return; }
    g_sceneSwapExe = exe;
    if (LoaderHook("extra large maps' physics stream swap under the world's lock", exe + 0xBB1430u, (void*)SceneSwapDetour, (void**)&g_sceneSwapOriginal))
        Note("difficulty: extra large maps: the physics stream swap could not be hooked; it stays unlocked");
}

static int NearCovered(int nc0, int nc1, int nb0, int nb1)
{
    const int H = 4096;
    int grid = g_ring.level == g_world.levels && g_ring.applied;
    for (int k = 0; k < NEAR_TILES_MAX; k++)
    {
        const NearHidden* q = &g_tileHidden[k];
        if (!g_tiles[k].live || g_tiles[k].level != g_world.levels || q->level != g_world.levels || !q->lowered) continue;
        int c0 = q->x0, c1 = q->x0 + q->n, b0 = H - 1 - (q->y0 + q->n), b1 = H - 1 - q->y0;
        if (nc0 >= c0 && nc1 <= c1 && nb0 >= b0 && nb1 <= b1) return 1;
    }
    if (!grid) return 0;
    const int B0 = H - 1 - g_ring.r0 - 512;
    int a0 = (int)floorf((float)(nc0 - g_ring.c0) / 512.0f), a1 = (int)floorf((float)(nc1 - 1 - g_ring.c0) / 512.0f);
    int b0 = (int)floorf((float)(nb0 - B0) / 512.0f), b1 = (int)floorf((float)(nb1 - 1 - B0) / 512.0f);
    if ((a1 - a0 + 1) * (b1 - b0 + 1) > 64) return 0;
    for (int a = a0; a <= a1; a++)
        for (int b = b0; b <= b1; b++)
        {
            int live = 0;
            for (int k = 0; k < NEAR_TILES_MAX && !live; k++)
                live = g_tiles[k].live && g_tiles[k].level == g_world.levels && g_tiles[k].cell[0] == a && g_tiles[k].cell[1] == b &&
                    g_tileHidden[k].level == g_world.levels && g_tileHidden[k].lowered;
            if (!live) return 0;
        }
    return 1;
}

static void NearHideUnion(uint64_t world, const char* why)
{
    uint64_t h = NearCoarseHandle();
    uintptr_t cow = 0, nodes = 0; uint32_t count = 0;
    if (h == 0 || SafeRead(h + 0x40, &cow, 8) || cow == 0 || SafeRead(cow, &count, 4) || SafeRead(cow + 8, &nodes, 8) || nodes == 0 || count == 0 || count > 1u << 20)
    { Note("difficulty: near terrain: the coarse terrain's bounds were not copied; it is still drawn under the tiles (%s)", why); return; }
    /* the game's edits write the tree under this lock (0x1ADCD0) and run NearBoundsFix in it */
    int locked = world != 0 && g_nearTerrainLock[0] != NULL && g_nearTerrainLock[1] != NULL;
    if (locked) g_nearTerrainLock[0](world + 0x230520u);
    if (g_hide.level != g_world.levels || g_hide.nodes != nodes || g_hide.total != count)
    {
        if (g_hide.hidden != NULL) VirtualFree(g_hide.hidden, 0, MEM_RELEASE);
        if (g_hide.saved != NULL) VirtualFree(g_hide.saved, 0, MEM_RELEASE);
        if (g_hide.level == g_world.levels && g_hide.nodes != 0) Note("difficulty: near terrain: the coarse terrain's bounds moved (0x%llX -> 0x%llX); hidden again from scratch",
            (unsigned long long)g_hide.nodes, (unsigned long long)nodes);
        g_hide.handle = 0; /* the detour leaves the tree alone while it is set up */
        g_hide.hidden = (uint8_t*)VirtualAlloc(NULL, count, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_hide.saved = (float*)VirtualAlloc(NULL, (size_t)count * 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_hide.level = g_world.levels; g_hide.nodes = nodes; g_hide.total = count; g_hide.count = 0;
        if (g_hide.hidden == NULL || g_hide.saved == NULL) { g_hide.level = -1; if (locked) g_nearTerrainLock[1](world + 0x230520u); return; }
    }
    g_hide.handle = h;
    /* first the tree as it should be with the nodes hidden so far (an edit since may have NaN'd an ancestor), so a node
     * hidden now saves real bounds */
    int before = NearBoundsFixAll(h);
    const int W = 4096, H = 4096;
    struct { uint32_t node; int c0, c1, b0, b1; } stack[128];
    int top = 0, hid = 0, shown = 0;
    stack[top].node = 0; stack[top].c0 = 0; stack[top].c1 = W; stack[top].b0 = 0; stack[top].b1 = H; top++;
    while (top > 0)
    {
        top--;
        uint32_t k = stack[top].node;
        int nc0 = stack[top].c0, nc1 = stack[top].c1, nb0 = stack[top].b0, nb1 = stack[top].b1;
        if (k >= count) continue;
        uint8_t* node = (uint8_t*)(uintptr_t)(nodes + (uint64_t)k * 24);
        if (NearCovered(nc0, nc1, nb0, nb1))
        {
            if (!g_hide.hidden[k]) { memcpy(g_hide.saved + 2 * (size_t)k, node, 8); g_hide.hidden[k] = 1; g_hide.count++; hid++; }
            continue; /* the subtree goes with it */
        }
        if (g_hide.hidden[k]) { memcpy(node, g_hide.saved + 2 * (size_t)k, 8); g_hide.hidden[k] = 0; g_hide.count--; shown++; }
        uint32_t child[4];
        memcpy(child, node + 8, 16);
        if (child[0] == 0xFFFFFFFFu) continue;
        int cm = (nc0 + nc1) / 2, bm = (nb0 + nb1) / 2;
        const int box[4][4] = {{nc0, cm, nb0, bm}, {cm, nc1, nb0, bm}, {cm, nc1, bm, nb1}, {nc0, cm, bm, nb1}};
        for (int q = 0; q < 4 && top < 128; q++)
        {
            stack[top].node = child[q]; stack[top].c0 = box[q][0]; stack[top].c1 = box[q][1]; stack[top].b0 = box[q][2]; stack[top].b1 = box[q][3];
            top++;
        }
    }
    int after = NearBoundsFixAll(h); /* the hidden nodes NaN, their ancestors their children's union */
    if (locked) g_nearTerrainLock[1](world + 0x230520u);
    Note("difficulty: near terrain: the coarse terrain under the %d live tile(s) hidden (%s): %d nodes hidden now (%d new, %d shown again); bounds put right: %d "
        "nodes before, %d after%s; edits repaired since the level began: %lld (%lld nodes)", NearTilesLive(), why, g_hide.count, hid, shown, before, after,
        locked ? "" : " (not under the game's terrain lock)", (long long)g_hide.fixes, (long long)g_hide.fixed);
}


/* Level layers on the near tile. A stamp's content is placed on the level's layer heightfield (4 m samples over the
 * 16 km world, the coarse heights), so under the near tile its shrubs, trees and rocks floated or sank by the difference
 * (user report on s70j). Every layer height read goes through the descriptor the layer spawn passes on (data +0,
 * origin = the grid's centre +8/+0xC, extent +0x14/+0x18, columns +0x1C, rows +0x20; a sample is
 * col = (int)(((x - origin) / extent + 0.5) x columns), row = rows - 1 - (int)(((y - origin) / extent + 0.5) x rows),
 * nearest, no interpolation: exe 0xD5B10 and the inline read in 0xDFA60 at 0xE058E..0xE0612):
 *  - placed units: 0xDFA60(rcx = the stamp's level unit, edx/r8d an index range, r9 = the descriptor), called only from
 *    0xDBEC0 (0xDC314), reads the grid inline (and through its holder in the aligned mode, 0xD6040). A stamp whose
 *    level unit stands within NEAR_LAYER_REACH of the tile (its translation at unit+0x50/+0x54, row 3 of the 4x4 pose
 *    at +0x20, as SpawnStamps 0xF44890 builds it; a footprint is at most 256 m, 362 m across) gets a 1 m grid of the
 *    hidden square and NEAR_LAYER_MARGIN around it: the tile's heights inside, the coarse terrain's outside (over the
 *    tile's buried band too).
 *  - the other members (SpeedTree, prefabs, volumes, ...): 0xDBEC0 samples 0xD5B10(holder, x, y) directly; inside
 *    the tile it returns the tile's height there (bilinear). */
#define NEAR_LAYER_MARGIN 768.0f
#define NEAR_LAYER_REACH 384.0f
static struct
{
    LONG level;
    float* data;
    uint32_t n;
    float side, cx, cy;
    uint8_t desc[0x28];
    volatile LONG descState;    /* 0 not made, 1 being made, 2 ready (layer spawns run on worker threads) */
    volatile LONG64 units, samples;
    volatile LONG notes;
} g_nearLayer = {.level = -1};


/* The ground as drawn: the tile inside the hidden square; past it, over the tile's band, the higher of the tile and the
 * coarse terrain (the band leaves the coarse surface NEAR_BAND_FLAT out and dives under it; the coarse terrain's ring
 * is lower there); -9999 elsewhere (the coarse terrain alone: nothing to change). */
static float NearVisibleAt(float x, float y)
{
    float best = -9999.0f;
    int inside = 0;
    AcquireSRWLockShared(&g_tilesLock);
    for (int k = 0; k < NEAR_TILES_MAX; k++)
    {
        const NearTile* t = &g_tiles[k];
        if (!t->live || t->level != g_world.levels) continue;
        float dx = fabsf(x - t->centre[0]), dy = fabsf(y - t->centre[1]);
        if (dx <= NEAR_HIDE / 2 && dy <= NEAR_HIDE / 2) { best = NearGroundAt(&t->ground, x, y); inside = 1; break; }
        if (dx > NEAR_SPAN / 2 || dy > NEAR_SPAN / 2) continue;
        float h = NearGroundAt(&t->ground, x, y);
        if (h > best) best = h;
    }
    ReleaseSRWLockShared(&g_tilesLock);
    if (inside || best < -9000.0f) return best;
    float c = NearGroundAt(&g_nearCoarseGround, x, y);
    return c < -9000.0f || best >= c ? best : c;
}

/* ---- A stratagem beacon's water depth over the near tiles --------------------------------------------------------
 * A beacon's deploy (0x6A2A30, at contact) rejects it when the water there is deeper than 1 m (0x6A2D8A: 0x13CA650 >
 * 1.0, every stratagem; the refund 0x66EE10 and the beacon destroyed). 0x13CA650(-, pos): 0 when [[game+0x33266A8]+0x2BC]
 * > 0 or the water manager [game+0x347CE48] has +0x44 clear; else water - ground, water = 0x13D4620(manager, pos,
 * G+0x119FF48) and ground = the hit of a ray from z + 500 m, 1000 m down, against the coarse terrain [G+0x8758A0] only,
 * when the level's +0x1E4 is 3 (0 otherwise, and 0 when the ray misses). Under every live near tile the coarse collision
 * is at -500 m (NearHideCoarse), so there the ray misses (ground 0) or hits -500 m, and the depth is the water level or
 * 500 m more: a beacon on dry ground was rejected wherever the water plane is above 1 m. Over a live tile the ground is
 * the tile's as drawn (NearVisibleAt); elsewhere, and in every other case, the game's own value. Its callers read only
 * xmm0 and their own saved registers after it (0x6A2B40, 0x8FA9FB, 0x8FB1B3, 0x11A1C93, the ground search 0x13CAA80). */
typedef float (*BeaconDepthFn)(uint64_t unused, const float* pos);
typedef float (*WaterLevelFn)(uintptr_t manager, const float* pos, uintptr_t mask);
static BeaconDepthFn g_beaconDepth;
static volatile LONG g_beaconDepthNotes;

/* The parts of the depth at pos: 1 when the tile's ground applies (*ground, *water set), 0 when the game's own value
 * stands (*kind the level's +0x1E4, or 0 when it did not read). */
static int BeaconDepthParts(const float* pos, float* ground, float* water, uint32_t* kind)
{
    uintptr_t game = g_world.game, holder = 0, manager = 0, gameObj = 0, lg = 0, gen = 0;
    uint32_t blocked = 0; uint8_t on = 0;
    *kind = 0;
    if (game == 0) return 0;
    float g = NearVisibleAt(pos[0], pos[1]);
    if (g < -9000.0f) return 0;
    if (SafeRead(game + 0x33266A8u, &holder, 8) || holder == 0 || SafeRead(holder + 0x2BC, &blocked, 4) || blocked != 0 ||
        SafeRead(game + 0x347CE48u, &manager, 8) || manager == 0 || SafeRead(manager + 0x44, &on, 1) || on == 0 ||
        SafeRead(game + 0x3326340u, &gameObj, 8) || gameObj == 0 || SafeRead(gameObj + 0xAE288, &lg, 8) || lg == 0 ||
        SafeRead(lg + 0x1E4, kind, 4) || *kind != 3 || SafeRead(lg + 0x2C8, &gen, 8) || gen == 0)
        return 0;
    float at[3] = {pos[0], pos[1], pos[2]};
    *water = ((WaterLevelFn)(game + 0x13D4620u))(manager, at, gen + 0x119FF48u);
    *ground = g;
    return 1;
}

static float BeaconDepth(uint64_t unused, const float* pos)
{
    float depth = g_beaconDepth(unused, pos), ground, water;
    uint32_t kind;
    if (!BeaconDepthParts(pos, &ground, &water, &kind)) return depth;
    float fixed = water - ground;
    if ((depth > 1.0f) != (fixed > 1.0f) && InterlockedIncrement(&g_beaconDepthNotes) <= 24)
        Note("difficulty: water depth at (%.1f, %.1f, %.1f) over a near tile: %.2f m with the ground as drawn (%.2f m, water %.2f m), the coarse ray's %.2f m",
            (double)pos[0], (double)pos[1], (double)pos[2], (double)fixed, (double)ground, (double)water, (double)depth);
    return fixed;
}

static void InstallBeaconDepth(uintptr_t game)
{
    if (ApiCodeMatches(game + 0x13CA650u, "40 53 48 81 EC A0 00 00 00"))
        LoaderHook("a stratagem beacon's water depth (the near tiles' ground)", game + 0x13CA650u, (void*)BeaconDepth, (void**)&g_beaconDepth);
    else Note("difficulty: the water depth query is not the reviewed one: stratagem beacons over the near tiles keep the coarse terrain's ground");
}

/* The live tile nearest to (x, y) whose terrain span reaches within `reach` of it; -1 when none. */
static int NearTileNear(float x, float y, float reach)
{
    int at = -1;
    AcquireSRWLockShared(&g_tilesLock);
    for (int k = 0; k < NEAR_TILES_MAX && at < 0; k++)
        if (g_tiles[k].live && g_tiles[k].level == g_world.levels && fabsf(x - g_tiles[k].centre[0]) <= reach + NEAR_SPAN / 2 &&
            fabsf(y - g_tiles[k].centre[1]) <= reach + NEAR_SPAN / 2) at = k;
    ReleaseSRWLockShared(&g_tilesLock);
    return at;
}

static void NearLayerBuild(void)
{
    const float side = NEAR_HIDE + 2.0f * NEAR_LAYER_MARGIN;
    const uint32_t n = (uint32_t)side; /* 1 m samples */
    if (g_nearLayer.data == NULL) g_nearLayer.data = (float*)VirtualAlloc(NULL, (size_t)n * n * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_nearLayer.data == NULL || g_nearTileGround.v == NULL || g_nearCoarseGround.v == NULL) { g_nearLayer.level = -1; return; }
    const float cx = g_near.centre[0], cy = g_near.centre[1], step = side / (float)n;
    uint32_t coarse = 0;
    for (uint32_t r = 0; r < n; r++)
    {
        float y = cy + side / 2 - ((float)r + 0.5f) * step;
        float* row = g_nearLayer.data + (size_t)r * n;
        for (uint32_t c = 0; c < n; c++)
        {
            float x = cx - side / 2 + ((float)c + 0.5f) * step;
            float h = NearVisibleAt(x, y);
            if (h < -9000.0f) { h = NearGroundAt(&g_nearCoarseGround, x, y); coarse++; }
            row[c] = h < -9000.0f ? 0.0f : h;
        }
    }
    g_nearLayer.n = n; g_nearLayer.side = side; g_nearLayer.cx = cx; g_nearLayer.cy = cy;
    g_nearLayer.units = g_nearLayer.samples = 0; g_nearLayer.notes = 0;
    memset(g_nearLayer.desc, 0, sizeof g_nearLayer.desc);
    InterlockedExchange(&g_nearLayer.descState, 0);
    g_nearLayer.level = g_world.levels;
    Note("difficulty: near terrain: level layers get a %u m grid of the tile and %.0f m around it at 1 m (%u samples of the coarse terrain)", n,
        (double)NEAR_LAYER_MARGIN, coarse);
}

/* The level's layer heightfield: its extent is the world's. */
static int NearLayerIsLevel(const uint8_t* desc)
{
    float extent[2]; memcpy(extent, desc + 0x14, 8);
    float world = 2048.0f * g_world.current;
    return fabsf(extent[0] - world) < 1.0f && fabsf(extent[1] - world) < 1.0f;
}

static int NearLayerOn(void)
{
    return g_nearLayer.level == g_world.levels && g_near.level == g_world.levels && g_near.state == NEAR_IN && g_nearLayer.data != NULL;
}

/* WorldLayerSnap (0xDFA60): the descriptor to pass instead of `field`, or 0 to keep it. */
static uint64_t NearLayerField(uint64_t unit, uint64_t field)
{
    if (!NearLayerOn() || unit == 0 || field == 0) return 0;
    uint8_t desc[0x28]; float t[2];
    if (SafeRead(field, desc, sizeof desc) || !NearLayerIsLevel(desc) || SafeRead(unit + 0x50, t, 8) || !isfinite(t[0]) || !isfinite(t[1])) return 0;
    float reach = NEAR_HIDE / 2 + NEAR_LAYER_REACH;
    if (fabsf(t[0] - g_near.centre[0]) > reach || fabsf(t[1] - g_near.centre[1]) > reach) return 0;
    if (g_nearLayer.descState != 2)
    {
        if (InterlockedCompareExchange(&g_nearLayer.descState, 1, 0) != 0) return 0; /* another thread is making it: keep the level's */
        uint8_t mine[0x28]; memcpy(mine, desc, sizeof mine);
        uint64_t data = (uint64_t)(uintptr_t)g_nearLayer.data;
        float origin[2] = {g_nearLayer.cx, g_nearLayer.cy}, extent[2] = {g_nearLayer.side, g_nearLayer.side};
        uint32_t dims[2] = {g_nearLayer.n, g_nearLayer.n};
        memcpy(mine, &data, 8); memcpy(mine + 8, origin, 8); memcpy(mine + 0x14, extent, 8); memcpy(mine + 0x1C, dims, 8);
        memcpy(g_nearLayer.desc, mine, sizeof mine); /* first use: the level's descriptor with our grid */
        MemoryBarrier();
        InterlockedExchange(&g_nearLayer.descState, 2);
    }
    LONG64 k = InterlockedIncrement64(&g_nearLayer.units);
    if (k == 1 || k == 100 || k == 1000)
        Note("difficulty: near terrain: level layers: %lld unit spawns on the tile's grid so far (this stamp at %.1f, %.1f)", (long long)k, (double)t[0], (double)t[1]);
    return (uint64_t)(uintptr_t)g_nearLayer.desc;
}

/* WorldLayerSample (0xD5B10): 1 with the tile's height at (x, y) when the descriptor is the level's and the point is in
 * the hidden square (or the tile grid's own descriptor: the aligned mode's holder holds ours). */
static int NearLayerHeight(uint64_t desc, float x, float y, float* height)
{
    if (!NearLayerOn() || desc == 0) return 0;
    uint8_t d[0x28];
    if (desc != (uint64_t)(uintptr_t)g_nearLayer.desc && (SafeRead(desc, d, sizeof d) || !NearLayerIsLevel(d))) return 0;
    float h = NearVisibleAt(x, y);
    if (h < -9000.0f) return 0;
    *height = h;
    LONG64 k = InterlockedIncrement64(&g_nearLayer.samples);
    if (k == 1 || k == 10000 || k == 1000000)
        Note("difficulty: near terrain: level layers: %lld height samples from the tile so far (%.1f m at %.1f, %.1f)", (long long)k, (double)h, (double)x, (double)y);
    return 1;
}

/* Where the tile meets the coarse terrain: along each edge of the hidden square, every 2 m, the tile's height against
 * the coarse surface at the same point (each from its own u16 copy, which the render and the collision use). */
static void NearSeamReport(void)
{
    static const char* const names[4] = {"west", "east", "south", "north"};
    const float half = NEAR_HIDE / 2, cx = g_near.centre[0], cy = g_near.centre[1];
    double all = 0; float worst = 0; int allN = 0, over1 = 0, over5 = 0;
    char text[512]; int len = 0;
    for (int side = 0; side < 4; side++)
    {
        double sum = 0, abs = 0; float big = 0; int n = 0;
        for (float t = -half; t <= half; t += 2.0f)
        {
            float x = side == 0 ? cx - half : side == 1 ? cx + half : cx + t, y = side == 2 ? cy - half : side == 3 ? cy + half : cy + t;
            float a = NearGroundAt(&g_nearTileGround, x, y), b = NearGroundAt(&g_nearCoarseGround, x, y);
            if (a < -9000 || b < -9000) continue;
            if (g_ring.level == g_world.levels && g_ring.applied) b += NEAR_RING_DIP; /* the coarse surface before its ring was lowered */
            float d = a - b;
            sum += d; abs += fabsf(d); n++;
            if (fabsf(d) > big) big = fabsf(d);
            over1 += fabsf(d) > 1.0f; over5 += fabsf(d) > 5.0f;
        }
        if (n == 0) continue;
        all += abs; allN += n; if (big > worst) worst = big;
        len += snprintf(text + len, sizeof text - len, "%s%s mean %+.2f, mean |d| %.2f, max %.2f", side ? "; " : "", names[side], sum / n, abs / n, (double)big);
    }
    if (allN == 0) { Note("difficulty: near terrain: seam: the terrains' heights did not read"); return; }
    Note("difficulty: near terrain: seam (tile - coarse at the hidden square's edge, every 2 m, %d points): mean |d| %.2f m, max %.2f, %d over 1 m, %d over 5 m; %s",
        allN, all / allN, (double)worst, over1, over5, text);
}

/* HELLFORGE_XL_NEAR_DUMP=1: both terrains' u16 heights into the session's logs (near-tile.u16, near-coarse.u16; 32 MB each)
 * with near-dump.txt (sizes, centres), for offline analysis. Test runs only. */
static void NearDumpGrounds(void)
{
    char t[4] = "";
    if (!(GetEnvironmentVariableA("HELLFORGE_XL_NEAR_DUMP", t, sizeof t) > 0 && t[0] == '1')) return;
    const NearGround* both[2] = {&g_nearTileGround, &g_nearCoarseGround};
    const wchar_t* files[2] = {L"near-tile.u16", L"near-coarse.u16"};
    char text[300]; int len = 0;
    for (int k = 0; k < 2; k++)
    {
        if (both[k]->v == NULL) continue;
        HANDLE f = OpenLog(files[k]);
        if (f == INVALID_HANDLE_VALUE) continue;
        uint64_t bytes = (uint64_t)both[k]->n * both[k]->n * 2, at = 0; DWORD wrote = 0;
        while (at < bytes) { DWORD chunk = (DWORD)(bytes - at > 0x10000000u ? 0x10000000u : bytes - at); if (!WriteFile(f, (const uint8_t*)both[k]->v + at, chunk, &wrote, NULL) || !wrote) break; at += wrote; }
        CloseHandle(f);
        len += snprintf(text + len, sizeof text - len, "%ls n %u size %.2f centre %.2f %.2f\n", files[k], both[k]->n, (double)both[k]->size, (double)both[k]->cx, (double)both[k]->cy);
    }
    HANDLE f = OpenLog(L"near-dump.txt");
    if (f != INVALID_HANDLE_VALUE) { DWORD wrote; WriteFile(f, text, (DWORD)len, &wrote, NULL); CloseHandle(f); }
    Note("difficulty: near terrain: both terrains' heights written (HELLFORGE_XL_NEAR_DUMP)");
}

/* The tile's own height-bounds quadtree, for the eye's A/B: it exists once an edit has run on the tile (0x2F63D0 copies
 * the nodes to [[h+0x40]+8] on the first edit), so one sample is written with the value it already has. */
static void NearTileBounds(uint64_t world)
{
    typedef void (*EditFn)(uint64_t world, uint64_t h, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const uint16_t* heights, const uint32_t* materials,
        uint32_t w, uint32_t rows);
    EditFn edit = (EditFn)NearApi(0x08, 0x1D0);
    const NearGround* t = &g_nearTileGround;
    uintptr_t cow = 0, nodes = 0; uint32_t count = 0;
    if (edit == NULL || t->v == NULL) return;
    uint32_t mid = t->n / 2;
    uint16_t same = t->v[(size_t)mid * t->n + mid];
    edit(world, g_near.handle, (int32_t)mid, (int32_t)mid, (int32_t)mid + 1, (int32_t)mid + 1, &same, NULL, 1, 1);
    if (SafeRead(g_near.handle + 0x40, &cow, 8) || cow == 0 || SafeRead(cow, &count, 4) || SafeRead(cow + 8, &nodes, 8) || nodes == 0 || count == 0)
    { Note("difficulty: near terrain: the tile's bounds were not copied (no A/B)"); return; }
    g_nearShown.tileRoot = (uint8_t*)(uintptr_t)nodes;
    memcpy(g_nearShown.tileBounds, g_nearShown.tileRoot, 8);
}

/* The A/B (research): 0 = as the tile is meant to be (the coarse nodes under it NaN); 1 "coarse" = the coarse terrain
 * drawn where it was hidden and the tile not drawn; 2 "both" = both drawn; 3 "low" = the coarse nodes hidden by finite
 * bounds far below (min = max = -100000) instead of NaN, the tile drawn. */
/* The hidden nodes are the coarse terrain's own memory, which the game frees when the mission ends while the loader's
 * level count only moves at the next level (s70l: a read at +0x662ec faulted ~10 s after the players were gone). So the
 * eye's A/B and the crater report touch them only while the game is in a mission (state 4: [[game + 0x3326340] +
 * 0xAC21C], set by ChangeState 0xAB2FA0), the chain level -> generator -> terrain -> bounds still leads to the array
 * NearHideCoarse edited, and its first and last node still read. */
static int NearShownLive(void)
{
    uintptr_t gameObj = 0, gen = 0, h = 0, cow = 0, nodes = 0; uint32_t state = 0, count = 0; uint8_t probe[24];
    if (g_nearShown.level != g_world.levels || g_nearShown.nodes == 0 || g_world.game == 0) return 0;
    if (SafeRead(g_world.game + 0x3326340u, &gameObj, 8) || gameObj == 0 || SafeRead(gameObj + 0xAC21Cu, &state, 4) || state != 4) return 0;
    if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0 || SafeRead(gen + 0x8758A0u, &h, 8) || h == 0 || SafeRead(h + 0x40, &cow, 8) || cow == 0 ||
        SafeRead(cow, &count, 4) || SafeRead(cow + 8, &nodes, 8)) return 0;
    if (nodes != g_nearShown.nodes || count != g_nearShown.count) return 0;
    return !SafeRead(nodes, probe, sizeof probe) && !SafeRead(nodes + (uint64_t)(count - 1) * 24, probe, sizeof probe);
}

static void NearShowCoarse(int mode)
{
    if (g_nearShown.level != g_world.levels || g_nearShown.tileRoot == NULL || mode == g_nearShown.coarseShown || !NearShownLive()) return;
    const float nan = __builtin_nanf(""), low = -100000.0f;
    for (int k = 0; k < g_nearShown.coarseCount; k++)
    {
        if (mode == 1 || mode == 2) memcpy(g_nearShown.coarseNode[k], g_nearShown.coarseBounds[k], 8);
        else { const float* v = mode == 3 ? &low : &nan; memcpy(g_nearShown.coarseNode[k], v, 4); memcpy(g_nearShown.coarseNode[k] + 4, v, 4); }
    }
    if (mode == 1) { memcpy(g_nearShown.tileRoot, &nan, 4); memcpy(g_nearShown.tileRoot + 4, &nan, 4); }
    else memcpy(g_nearShown.tileRoot, g_nearShown.tileBounds, 8);
    g_nearShown.coarseShown = mode;
    static const char* const names[4] = {"the tile shown, the coarse terrain hidden under it (NaN bounds)", "the coarse terrain shown, the tile hidden",
        "both terrains shown", "the tile shown, the coarse terrain hidden by bounds far below"};
    Note("difficulty: near terrain: eye A/B: %s", names[mode]);
}

/* ---- The test eye (HELLFORGE_XL_EYE=1; test runs only) ------------------------------------------------------------
 * The gameplay camera is set natively each frame by three callers in game.dll, each the same way (game 0x1279190's
 * pattern: [state] the camera, [state+8] its node, set_local_position [api+0x20]+0x20 then set_local_rotation +0x28 then
 * [api+8]+0x100 to update the unit): positions return to 0x1279297 / 0x127A139 / 0x127AAAE, rotations to 0x12792B0 /
 * 0x127A152 / 0x127AACA. While the file says so, those calls get the eye's pose instead, and that camera a 20 km far range.
 * The file, xl-eye.txt in the session's logs (HELLFORGE_XL_EYE=1; polled twice a second): "x y h tx ty th [coarse]": the eye at world (x, y) h metres above the ground there,
 * looking at (tx, ty) th above the ground there; "coarse" = the A/B (NearShowCoarse); "stream" = the content streamer
 * also counts (tx, ty) as a player, so the content there spawns; anything else = the game's camera.
 * The rotation is built as SetupTerrainMerge builds its cameras' (0xF487B0): exe-independent game.dll helpers 0x173F150
 * (out 4x4, forward, up: a look rotation; normalizes forward in place) and 0x173A750 (quaternion out, 4x4). */
static struct { int on, coarse; float pos[3], rot[4], game[3]; uint64_t camera; ULONGLONG checked; FILETIME stamp; int pinned; } g_eye; /* game: the game's own camera position, last frame */

/* The ground the eye stands on: the surface drawn there (NearVisibleAt: the tile inside the hidden square, the higher of
 * the tile's buried band and the coarse terrain past it), the coarse terrain off the tile or while the A/B shows it. */
static float NearEyeGround(float x, float y)
{
    float a = g_nearShown.coarseShown == 1 ? -9999.0f : NearVisibleAt(x, y);
    return a > -9000 ? a : NearGroundAt(&g_nearCoarseGround, x, y);
}

/* Research (the test eye): the Terrain Deformation System's craters and the hidden coarse nodes they un-hid. S =
 * [game+0x347CE58] (loaded at 0x1387372 for the server 0x1386620); records u64 at S+0x800E0, count u32 at S+0x2800E0
 * (append 0x1386580), type 7 at S+0x2801E0 (count S+0x4801E0). d0 = x<<20 | y<<8 | angle, d1 = z<<22 | type<<16 |
 * depthfix<<8 | diameter; the server decodes x, y as texel x 0.5 - 1024 m (0x23C6AD0, 0x23C7D58) and z - 512 (0x23C7B88):
 * a vanilla 2048 m / 4096-texel map. On this map's 4096-texel level textures (4 m a texel) texel t is at 4 t - 8192. */
static void NearCraterReport(void)
{
    uint64_t s = 0;
    if (SafeRead(g_world.game + 0x347CE58u, &s, 8) || s == 0) { Note("difficulty: near terrain: craters: no deformation system"); return; }
    static uint32_t seen[2]; static LONG level = -1;
    if (level != g_world.levels) { level = g_world.levels; seen[0] = seen[1] = 0; }
    for (int list = 0; list < 2; list++)
    {
        uint64_t base = s + (list ? 0x2801E0u : 0x800E0u);
        uint32_t count = 0;
        if (SafeRead(base + 0x200000u, &count, 4)) continue;
        if (count > 0x40000u) count = 0x40000u;
        for (uint32_t i = seen[list]; i < count && i < seen[list] + 64; i++)
        {
            uint32_t d[2];
            if (SafeRead(base + (uint64_t)i * 8, d, 8)) break;
            uint32_t x = d[0] >> 20, y = (d[0] >> 8) & 0xFFF;
            Note("difficulty: near terrain: crater %s%u: texel (%u, %u), as vanilla (%.1f, %.1f), on this map (%.0f, %.0f); z %d, type %u, depthfix %u, "
                "diameter byte %u (%.1f m vanilla, %.0f m here)", list ? "t7-" : "", i, x, y, x * 0.5 - 1024.0, y * 0.5 - 1024.0, x * 4.0 - 8192.0,
                y * 4.0 - 8192.0, (int)(d[1] >> 22) - 512, (d[1] >> 16) & 0x3F, (d[1] >> 8) & 0xFF, d[1] & 0xFF, (d[1] & 0xFF) * 0.5, (d[1] & 0xFF) * 4.0);
        }
        if (count != seen[list]) Note("difficulty: near terrain: craters: list %d holds %u", list, count);
        seen[list] = count;
    }
    /* the drawn heights against the CPU copies: the level's live global map (by name, as the crater readbacks look it
     * up) vs the coarse copy, then the tile's texture vs the tile's copy; each GPU image written for offline analysis */
    static struct { LONG level; int which, mip, state; uint32_t id, polls; } gpu = {-1, 0, 0, 0, 0, 0};
    if (gpu.level != g_world.levels) { gpu.level = g_world.levels; gpu.which = 0; gpu.mip = 0; gpu.state = 0; }
    typedef uint32_t (*MipReadbackFn)(uint64_t resource, uint16_t mip); /* [api+0x10]+0x1B0, as 0x5B uses it (game 0xC1742F) */
    NearFindFn find = (NearFindFn)NearApi(0x10, 0x158);
    MipReadbackFn readback = (MipReadbackFn)NearApi(0x10, 0x1B0);
    NearReadyFn ready = (NearReadyFn)NearApi(0x10, 0x1D0);
    NearDescFn desc = (NearDescFn)NearApi(0x10, 0x1D8);
    char dumpOn[4] = "";
    int research = GetEnvironmentVariableA("HELLFORGE_XL_NEAR_DUMP", dumpOn, sizeof dumpOn) > 0 && dumpOn[0] == '1';
    while (research && gpu.which < 2 && find && readback && ready && desc)
    {
        const NearGround* cpu = gpu.which == 0 ? &g_nearCoarseGround : &g_nearTileGround;
        if (gpu.mip > 4) { gpu.which++; gpu.mip = 0; gpu.state = 0; continue; }
        if (gpu.state == 0)
        {
            uint64_t tex = gpu.which == 0 ? find(kNearNames[0]) : g_near.tex[0];
            uint32_t mipCount = 0;
            if (tex == 0 || cpu->v == NULL) { gpu.which++; gpu.mip = 0; continue; }
            /* the readback (exe 0x31B500) does not check the mip: only the texture's own ([tex+0x20]) */
            if (SafeRead(tex + 0x20, &mipCount, 4) || gpu.mip >= (int)mipCount) { gpu.which++; gpu.mip = 0; continue; }
            gpu.id = readback(tex, (uint16_t)gpu.mip); gpu.polls = 0; gpu.state = 1;
            break;
        }
        if (!ready(gpu.id)) { if (++gpu.polls > 40) { Note("difficulty: near terrain: drawn heights: readback %d mip %d did not come", gpu.which, gpu.mip); gpu.mip = 5; gpu.state = 0; } break; }
        uint8_t out[32] = {0}, d[24];
        const uint8_t* p = desc(out, gpu.id);
        memcpy(d, p ? p : out, 24);
        uint32_t w, hgt, format; uint64_t data;
        memcpy(&w, d, 4); memcpy(&hgt, d + 4, 4); memcpy(&format, d + 8, 4); memcpy(&data, d + 16, 8);
        static uint16_t* img;
        if (img == NULL) img = (uint16_t*)VirtualAlloc(NULL, (size_t)4096 * 4096 * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        uint32_t k = 4096u >> gpu.mip;
        if (img != NULL && data != 0 && w == k && hgt == k && NearTexelBytes(format) == 2 && !SafeRead(data, img, (size_t)k * k * 2))
        {
            /* against the CPU copy box-averaged to this mip (2^mip texels a side) */
            uint32_t f = 1u << gpu.mip; double sum = 0, signedSum = 0; float big = 0; uint32_t over1 = 0, over5 = 0;
            for (uint32_t y = 0; y < k; y++)
                for (uint32_t x = 0; x < k; x++)
                {
                    double avg = 0;
                    for (uint32_t j = 0; j < f; j++) for (uint32_t i = 0; i < f; i++) avg += cpu->v[(size_t)(y * f + j) * 4096 + x * f + i];
                    avg /= (double)(f * f);
                    float m = (float)(((double)img[(size_t)y * k + x] - avg) / 65535.0 * 1000.0);
                    signedSum += m; sum += fabsf(m); if (fabsf(m) > big) big = fabsf(m); over1 += fabsf(m) > 1.0f; over5 += fabsf(m) > 5.0f;
                }
            Note("difficulty: near terrain: drawn heights (%s, GPU mip %d, %u x %u) vs its CPU copy averaged: mean %+.3f m, mean |d| %.3f, max %.2f, %u texels over 1 m, "
                "%u over 5 m", gpu.which ? "the tile" : "the level's global map", gpu.mip, k, k, signedSum / ((double)k * k), sum / ((double)k * k), (double)big, over1, over5);
            wchar_t name[48];
            swprintf(name, 48, L"near-%ls-gpu-mip%d.u16", gpu.which ? L"tile" : L"coarse", gpu.mip);
            HANDLE fh = OpenLog(name);
            if (fh != INVALID_HANDLE_VALUE) { DWORD wrote; WriteFile(fh, img, k * k * 2u, &wrote, NULL); CloseHandle(fh); }
        }
        else Note("difficulty: near terrain: drawn heights: readback %d mip %d is %u x %u format %u", gpu.which, gpu.mip, w, hgt, format);
        gpu.mip++; gpu.state = 0;
    }
    int open = 0;
    if (g_nearShown.coarseShown == 0 && NearShownLive())
        for (int k = 0; k < g_nearShown.coarseCount; k++) { float b[2]; if (SafeRead((uintptr_t)g_nearShown.coarseNode[k], b, 8)) break; open += !(isnan(b[0]) && isnan(b[1])); }
    Note("difficulty: near terrain: craters: %d of %d hidden coarse nodes have finite bounds again", open, g_nearShown.coarseCount);
}

__declspec(dllexport) int hellforge_research_stream_observer(int on, float x, float y); /* difficulty_stream.c */
static void NearEyePoll(void)
{
    ULONGLONG now = GetTickCount64();
    if (now - g_eye.checked < 500) return;
    g_eye.checked = now;
    static wchar_t path[MAX_PATH]; static int have = -1, missing;
    if (have < 0)
    {
        char t[4] = "";
        have = GetEnvironmentVariableA("HELLFORGE_XL_EYE", t, sizeof t) > 0 && t[0] == '1' && g_logs[0] &&
            swprintf(path, MAX_PATH, L"%ls\\xl-eye.txt", g_logs) > 0;
        if (have) Note("difficulty: near terrain: eye: watching xl-eye.txt in the session's logs");
    }
    if (!have) return;
    if (g_eye.pinned == 0)
        g_eye.pinned = ApiCodeMatches(g_world.game + 0x173F150u, "48 8B C4 48 89 58 08 48 89 70 20 57 48 81 EC 90 00 00 00") &&
            ApiCodeMatches(g_world.game + 0x173A750u, "48 89 5C 24 20 57 48 83 EC 30 F3 0F 10 12") &&
            ApiCodeMatches(g_world.game + 0x1279293u, "41 FF 51 20 48 8B 05") && ApiCodeMatches(g_world.game + 0x12792ACu, "41 FF 51 28") &&
            ApiCodeMatches(g_world.game + 0x127A135u, "41 FF 51 20 48 8B 05") && ApiCodeMatches(g_world.game + 0x127A14Eu, "41 FF 51 28") &&
            ApiCodeMatches(g_world.game + 0x127AAACu, "FF D0 48 8B 05") && ApiCodeMatches(g_world.game + 0x127AAC8u, "FF D0") ? 1 : -1;
    if (g_eye.pinned < 0) { have = 0; Note("difficulty: near terrain: eye: the camera code is not the reviewed build; no eye"); return; }
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a)) { if (!missing) { missing = 1; Note("difficulty: near terrain: eye: no xl-eye.txt yet"); } if (g_eye.on) { g_eye.on = 0; NearShowCoarse(0); hellforge_research_stream_observer(0, 0.0f, 0.0f); Note("difficulty: near terrain: eye off"); } return; }
    if (CompareFileTime(&a.ftLastWriteTime, &g_eye.stamp) == 0) return;
    g_eye.stamp = a.ftLastWriteTime;
    char text[160] = ""; DWORD got = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    ReadFile(f, text, sizeof text - 1, &got, NULL); CloseHandle(f);
    text[got] = 0;
    float p[6]; char mode[16] = "";
    int fields = sscanf(text, "%f %f %f %f %f %f %15s", &p[0], &p[1], &p[2], &p[3], &p[4], &p[5], mode);
    if (fields < 6 || g_near.state != NEAR_IN || g_near.level != g_world.levels)
    {
        Note("difficulty: near terrain: eye: \"%.60s\" (%d fields; tile state %d): the game's camera", text, fields, g_near.state);
        if (g_eye.on) Note("difficulty: near terrain: eye off");
        g_eye.on = 0; NearShowCoarse(0); hellforge_research_stream_observer(0, 0.0f, 0.0f); return;
    }
    NearCraterReport();
    NearShowCoarse(fields < 7 ? 0 : strcmp(mode, "coarse") == 0 ? 1 : strcmp(mode, "both") == 0 ? 2 : strcmp(mode, "low") == 0 ? 3 : 0);
    float g0 = NearEyeGround(p[0], p[1]), g1 = NearEyeGround(p[3], p[4]);
    if (g0 < -9000 || g1 < -9000) { Note("difficulty: near terrain: eye: (%.0f, %.0f) or (%.0f, %.0f) is off the map", (double)p[0], (double)p[1], (double)p[3], (double)p[4]); return; }
    float eye[3] = {p[0], p[1], g0 + p[2]}, forward[3] = {p[3] - p[0], p[4] - p[1], g1 + p[5] - eye[2]}, up[3] = {0, 0, 1};
    float matrix[16], quat[4];
    typedef void (*LookFn)(float* out, float* forward, const float* up);
    typedef void (*QuatFn)(float* out, const float* matrix);
    ((LookFn)(g_world.game + 0x173F150u))(matrix, forward, up);
    ((QuatFn)(g_world.game + 0x173A750u))(quat, matrix);
    memcpy(g_eye.pos, eye, 12); memcpy(g_eye.rot, quat, 16);
    g_eye.on = 1;
    /* "stream": the content streamer counts the looked-at point as a player (its stamps within 500 m spawn there) */
    hellforge_research_stream_observer(fields >= 7 && strcmp(mode, "stream") == 0, p[3], p[4]);
    Note("difficulty: near terrain: eye at (%.1f, %.1f, %.1f) (ground %.1f) looking at (%.1f, %.1f, %.1f)%s", (double)eye[0], (double)eye[1], (double)eye[2],
        (double)g0, (double)p[3], (double)p[4], (double)(g1 + p[5]), g_nearShown.coarseShown == 1 ? "; the coarse terrain shown" : "");
}

/* WorldCamSetPosition / -Rotation / -Far: 1 when the eye replaced the value (out filled; far: the range to use). */
static int TerrainNearEyePosition(uint64_t camera, uintptr_t at, const float* v, float* out)
{
    uintptr_t g = g_world.game;
    if (at != g + 0x1279297u && at != g + 0x127A139u && at != g + 0x127AAAEu) return 0;
    if (v != NULL) memcpy(g_eye.game, v, 12);
    NearEyePoll();
    if (!g_eye.on) return 0;
    static int logged;
    if (!logged && v != NULL) { logged = 1; Note("difficulty: near terrain: eye: the game's camera was at (%.1f, %.1f, %.1f)", (double)v[0], (double)v[1], (double)v[2]); }
    g_eye.camera = camera;
    memcpy(out, g_eye.pos, 12);
    return 1;
}

static int TerrainNearEyeRotation(uintptr_t at, float* out)
{
    uintptr_t g = g_world.game;
    if (!g_eye.on || (at != g + 0x12792B0u && at != g + 0x127A152u && at != g + 0x127AACAu)) return 0;
    memcpy(out, g_eye.rot, 16);
    return 1;
}

static int TerrainNearEyeFar(uint64_t camera) { return g_eye.on && camera == g_eye.camera; }

/* The tile's spawn (InitSpawnTerrain on the kept record, the config table's slots holding the tile's textures for the
 * call): 1 when spawned (NEAR_READY), 0 when not (NEAR_FAILED). The level's own 0x6B (TerrainNearStep) and a tile built
 * in mission (difficulty_world_move.c) both come here. */
static int NearSpawnTile(uint64_t context)
{
    g_near.state = NEAR_FAILED;
    uintptr_t api = 0, table = 0, world = 0;
    if (SafeRead(g_world.game + 0x3326308u, &api, 8) || api == 0 || SafeRead(api + 8, &table, 8) || table == 0) { Note("difficulty: near terrain: no engine table"); return 0; }
    if (SafeRead(context + 0x40, &world, 8) || world == 0 || !NearSpawnSlots(world)) return 0;
    volatile uint64_t* slot = (volatile uint64_t*)(table + 0x198);
    DWORD old = 0;
    if (!VirtualProtect((void*)slot, 8, PAGE_READWRITE, &old)) { Note("difficulty: near terrain: the spawn slot is not writable"); return 0; }
    g_nearSpawnOriginal = (NearSpawnFn)(uintptr_t)*slot;
    *slot = (uint64_t)(uintptr_t)NearSpawn;
    g_nearSpawnThread = GetCurrentThreadId();
    g_near.handle = 0;
    memset(g_near.one69 + 8 + 0x60, 0, 8);
    typedef void (*InitFn)(uint64_t pass, uint64_t unused, uint64_t context);
    ((InitFn)(g_world.game + 0xF44DC0u))((uint64_t)(uintptr_t)g_near.one69 + 8, 0, context);
    g_nearSpawnThread = 0;
    g_nearSlots[0] = g_nearSlots[1] = NULL;
    *slot = (uint64_t)(uintptr_t)g_nearSpawnOriginal;
    VirtualProtect((void*)slot, 8, old, &old);
    memcpy(&g_near.handle, g_near.one69 + 8 + 0x60, 8);
    if (g_near.handle == 0) { Note("difficulty: near terrain: the tile was not spawned"); return 0; }
    Note("difficulty: near terrain: spawned the %.0f m tile at (%.0f, %.0f) with its own maps, handle 0x%llX; waiting for it", (double)NEAR_SIDE,
        (double)g_near.centre[0], (double)g_near.centre[1], (unsigned long long)g_near.handle);
    if (g_matband.tex != 0)
        Note("difficulty: near terrain: the tile's material map is the band's texture; its physics materials %s", g_matband.served == 1 ? "from the band's pixels" :
            g_matband.served == 2 ? "read back from its render target (the sync path)" : "were not asked for in the spawn call");
    g_near.state = NEAR_READY; g_near.polls = 0;
    return 1;
}

/* The tile ready (polled each call: 1 while waiting), then SpawnTerrainWork and the finalize with its body moved to the
 * tile; 0 when finalized (or failed: NEAR_FAILED). *world gets the world. */
static int NearReadyTile(uint64_t context, uint64_t* worldOut)
{
    uintptr_t api = 0, table = 0, world = 0;
    SafeRead(g_world.game + 0x3326308u, &api, 8); SafeRead(api + 8, &table, 8); SafeRead(context + 0x40, &world, 8);
    typedef char (*ReadyFn)(uint64_t world, uint64_t handle);
    typedef void (*WorkFn)(uint64_t world, uint64_t handle);
    ReadyFn ready = NULL; WorkFn work = NULL;
    SafeRead(table + 0x1A0, &ready, 8); SafeRead(table + 0x1A8, &work, 8);
    if (ready == NULL || work == NULL || world == 0) { g_near.state = NEAR_FAILED; return 0; }
    if (!ready(world, g_near.handle))
    {
        if (++g_near.polls < 3000) return 1;
        Note("difficulty: near terrain: the tile was not ready after %u polls; left as it is", g_near.polls);
        g_near.state = NEAR_FAILED; return 0;
    }
    work(world, g_near.handle);
    g_near.handleCell = g_near.handle;
    uint64_t cell = (uint64_t)(uintptr_t)&g_near.handleCell;
    memcpy(g_near.one6B + 8, &cell, 8);
    /* The collision body: finalize (exe 0x1AC780) adds it through 0x7868D0 at (-0.5 x obj+0x168, +0.5 x obj+0x16C,
     * -0.5 x (obj+0x174 - obj+0x170)) and never reads the unit's pose; SpawnTerrainWork copied those floats from
     * the entry (+0x24..+0x30) and built the shape before this. For the finalize call only, the two sizes say
     * sx - 2 cx and sy + 2 cy, which puts the body's corner where the tile's is; then they are put back. */
    uint64_t body = 0;
    float sizes[2] = {0}, shifted[2];
    {
        typedef uint8_t* (*FindFn)(uint64_t table, uint64_t* key);
        uint64_t key = g_near.handle;
        uint8_t* e = ((FindFn)((uintptr_t)GetModuleHandleW(NULL) + 0x1B86C0u))(world + 0x230550u, &key);
        if (e != NULL && !SafeRead((uintptr_t)e + 0x10, &body, 8) && body != 0 && !SafeRead(body + 0x168, sizes, 8) && sizes[0] > 0 && sizes[1] > 0)
        {
            shifted[0] = sizes[0] - 2.0f * g_near.centre[0]; shifted[1] = sizes[1] + 2.0f * g_near.centre[1];
            memcpy((void*)(uintptr_t)(body + 0x168), shifted, 8);
        }
        else body = 0;
    }
    if (body == 0) Note("difficulty: near terrain: the terrain object did not read; its collision stays at the centre");
    typedef char (*FinalizeFn)(uint64_t pass, uint64_t unused, uint64_t context);
    ((FinalizeFn)(g_world.game + 0xF451E0u))((uint64_t)(uintptr_t)g_near.one6B + 8, 0, context);
    if (body != 0) memcpy((void*)(uintptr_t)(body + 0x168), sizes, 8);
    Note("difficulty: near terrain: the %.0f m tile is in (%u polls); its ground at the centre %.2f m, the level's %.2f m", (double)NEAR_SIDE, g_near.polls,
        (double)NearTileAt(g_near.centre[0], g_near.centre[1]), (double)NearGlobalAt(g_near.centre[0], g_near.centre[1]));
    RenderCapacityNote();
    *worldOut = world;
    return 0;
}

static int TerrainNearStep(uint64_t records, uint32_t end, uint32_t at, uint32_t op, uint64_t context, uint64_t g, uint64_t h)
{
    NearOneMipDisarm(); /* armed for the level's 0x69 alone */
    if (!TerrainOn() || g_world.current != 8.0f) return 0;
    NearParamsLoad(); /* the edge's tunables, once per level, before its first op that uses them (the ring at 0x64) */
    if (op == 0x69 && g_coarseMat.tex != 0 && g_coarseMat.level == g_world.levels && !g_oneMip.spawned)
    {
        /* the interpolated map's texture made on the render side before the coarse terrain's first draw (as the band's) */
        if (g_coarseMat.holds < NEAR_MAT_HOLD || GetTickCount64() - g_coarseMat.made < 100) { g_coarseMat.holds++; return 1; }
        NearCoarseMatScatter();
    }
    if (op == 0x69 && g_oneMip.level == g_world.levels && g_oneMip.tex != 0 && !g_oneMip.spawned) NearOneMipArm(context);
    if (op == 0x3E) { NearKeepCaptures(context); return 0; }
    if (op == 0x64 && g_ring.level != g_world.levels && g_near.level != g_world.levels && NearOn() && NearRingStep(context)) return 1;
    if (op == 0x65) return TerrainNearReplay(records, end, at, context, g, h);
    if (g_near.level != g_world.levels) return 0;
    if (op == 0x69 && g_near.state == NEAR_SPAWN && g_near.at69 != at)
    {
        g_near.at69 = at; g_near.at6B = 0;
        uint32_t o = 0;
        if (SafeRead(records + (uint64_t)at * 0x19B0u, g_near.one69, sizeof g_near.one69)) { g_near.state = NEAR_FAILED; return 0; }
        for (uint32_t i = at + 1; i < end && i < at + 16; i++) if (!SafeRead(records + (uint64_t)i * 0x19B0u, &o, 4) && o == 0x6B) { g_near.at6B = i; break; }
        if (g_near.at6B == 0 || SafeRead(records + (uint64_t)g_near.at6B * 0x19B0u, g_near.one6B, sizeof g_near.one6B))
        { Note("difficulty: near terrain: no SpawnTerrainFinalize after record %u", at); g_near.state = NEAR_FAILED; return 0; }
        memcpy(g_near.keep69, g_near.one69, sizeof g_near.keep69); memcpy(g_near.keep6B, g_near.one6B, sizeof g_near.keep6B);
        return 0;
    }
    if (g_near.at6B == 0 || at <= g_near.at6B) return 0;
    if (g_near.state == NEAR_SPAWN)
    {
        /* the band's texture made on the render side before the tile's first draw (NearMatBuild) */
        if (g_matband.tex != 0 && (g_matband.holds < NEAR_MAT_HOLD || GetTickCount64() - g_matband.made < 100)) { g_matband.holds++; return 1; }
        return NearSpawnTile(context);
    }
    if (g_near.state == NEAR_READY)
    {
        uint64_t world = 0;
        if (NearReadyTile(context, &world)) return 1;
        if (g_near.state == NEAR_FAILED) return 0;
        NearTilesClear();
        {
            int x0 = 0, y0 = 0;
            NearCellSquare(0, 0, g_near.centre[0], g_near.centre[1], &x0, &y0);
            NearHideCoarse(world, x0, y0, &g_tileHidden[0]);
        }
        {
            uintptr_t gen = 0, h = 0;
            if (!SafeRead(g_worldLevel + 0x2C8, &gen, 8) && gen != 0 && !SafeRead(gen + 0x8758A0u, &h, 8))
                NearGroundOf(world, h, 16384.0f - 0.5f, 0.0f, 0.0f, &g_nearCoarseGround, &g_groundBuf[NEAR_TILES_MAX]);
            NearGroundOf(world, g_near.handle, NEAR_SPAN, g_near.centre[0], g_near.centre[1], &g_nearTileGround, &g_groundBuf[0]);
            if (g_nearTileGround.v != NULL && NearTileAdd(0, g_near.centre[0], g_near.centre[1], g_near.handle, &g_nearTileGround) == 0)
            {
                g_tiles[0].cell[0] = g_tiles[0].cell[1] = 0;
                g_tiles[0].tex[0] = g_tiles[0].tex[1] = g_tiles[0].band = 0; g_tiles[0].bandPixels = NULL; /* g_near's and g_matband's */
            }
            NearHideUnion(world, "the drop tile");
            /* the eye's A/B: what was hidden for the drop tile */
            g_nearShown.level = g_world.levels; g_nearShown.tileRoot = NULL; g_nearShown.coarseShown = 0; g_nearShown.coarseCount = 0;
            g_nearShown.nodes = g_hide.nodes; g_nearShown.count = g_hide.total;
            for (uint32_t k = 0; g_hide.level == g_world.levels && k < g_hide.total && g_nearShown.coarseCount < NEAR_HIDDEN_MAX; k++)
                if (g_hide.hidden[k])
                {
                    g_nearShown.coarseNode[g_nearShown.coarseCount] = (uint8_t*)(uintptr_t)(g_hide.nodes + (uint64_t)k * 24);
                    memcpy(g_nearShown.coarseBounds[g_nearShown.coarseCount], g_hide.saved + 2 * (size_t)k, 8);
                    g_nearShown.coarseCount++;
                }
            NearSeamReport();
            NearDumpGrounds();
            NearTileBounds(world);
            NearLayerBuild();
        }
        g_near.state = NEAR_IN;
        return 0;
    }
    return 0;
}

/* The samplers' wrappers, on any larger map (they only act while a tile builds). */
/* Scatter (grass, plants, small rocks) on the near tile. The brushes' callback game+0x137A2A0 fills a cell's instances
 * (0x30 each: x, y at +0 in world metres from the centre, z at +8; 0x137A9B9 / 0x137A9DD) and takes z from one image,
 * [[brush+0x2858]+0x10] = {cols, rows, .., u16 data at +0x10}, bilinear at u = x / world + 0.5, v = y / world + 0.5
 * with the rows flipped (0x137A8A3..0x137A9CD), h / 65535 x 1000 - 500: the level's coarse heights. Under the near tile
 * (its own 0.5 m terrain, the coarse one hidden) the ground is the tile's, so grass and rocks floated or sank by the
 * difference. Each instance inside the tile keeps the callback's own offsets and moves by tile - image at its point.
 * The engine calls it as fn(x, y, size, -, out, max, brush) (exe 0x213450..0x213479: r9 unused by the callback, out
 * at [rsp+0x20], max +0x28, brush +0x30; the callback writes through [rbp+0x180] and checks [rbp+0x188]). */
typedef uint32_t (*NearScatterFn)(int32_t x, int32_t y, int32_t size, void* unused, void* out, uint32_t max, uint8_t* brush);
static NearScatterFn g_nearScatter;
static volatile LONG64 g_nearScatterMoved, g_nearScatterSeen, g_nearScatterCalls, g_nearScatterNear, g_nearScatterNoTile, g_nearScatterNoImage;
static volatile LONG64 g_nearScatterNoted;
static volatile LONG g_nearScatterNotes;
static volatile LONG g_nearScatterLevel = -1;
static float NearScatterImage(uintptr_t image, float x, float y)
{
    uint32_t dims[2] = {0}; uint64_t data = 0;
    if (SafeRead(image, dims, 8) || SafeRead(image + 0x10, &data, 8) || data == 0 || dims[0] < 2 || dims[1] < 2 || dims[0] > 16384 || dims[1] > 16384) return -9999.0f;
    float world = 2048.0f * g_world.current, u = x / world + 0.5f, v = y / world + 0.5f;
    if (!(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)) return -9999.0f;
    uint32_t w = dims[0], h = dims[1];
    float fx = (float)(w - 1) * u, fy = (float)(h - 1) * v;
    uint32_t c0 = (uint32_t)fx, r0 = (uint32_t)fy, c1 = c0 + 1 < w - 1 ? c0 + 1 : w - 1, r1 = r0 + 1 < h - 1 ? r0 + 1 : h - 1;
    float ax = fx - (float)c0, ay = fy - (float)r0;
    uint64_t rowA = (uint64_t)(h - r0 - 1) * w, rowB = (uint64_t)(h - r1 - 1) * w;
    uint16_t a = 0, b = 0, c = 0, d = 0;
    if (SafeRead(data + (rowA + c0) * 2, &a, 2) || SafeRead(data + (rowA + c1) * 2, &b, 2) || SafeRead(data + (rowB + c0) * 2, &c, 2) || SafeRead(data + (rowB + c1) * 2, &d, 2))
        return -9999.0f;
    float top = ((float)a / 65535.0f) * (1.0f - ax) + ((float)b / 65535.0f) * ax, bottom = ((float)c / 65535.0f) * (1.0f - ax) + ((float)d / 65535.0f) * ax;
    return (top * (1.0f - ay) + bottom * ay) * 1000.0f - 500.0f;
}

static uint32_t NearScatterCell(int32_t x, int32_t y, int32_t size, void* unused, void* out, uint32_t max, uint8_t* brush)
{
    uint32_t made = g_nearScatter(x, y, size, unused, out, max, brush);
    if (g_nearScatterLevel != g_world.levels)
    {
        g_nearScatterLevel = g_world.levels;
        g_nearScatterMoved = g_nearScatterSeen = g_nearScatterCalls = g_nearScatterNear = g_nearScatterNoTile = g_nearScatterNoImage = 0;
        g_nearScatterNoted = (LONG64)GetTickCount64(); g_nearScatterNotes = 0;
    }
    InterlockedIncrement64(&g_nearScatterCalls);
    {
        LONG64 now = (LONG64)GetTickCount64(), last = g_nearScatterNoted;
        if (now - last > 15000 && g_nearScatterNotes < 8 && InterlockedCompareExchange64(&g_nearScatterNoted, now, last) == last)
        {
            g_nearScatterNotes++;
            Note("difficulty: near terrain: scatter so far: %lld cells asked, %lld near the tile (tile state %d), %lld instances on it (%lld moved over 5 cm), %lld off the hidden square, %lld without the image",
                (long long)g_nearScatterCalls, (long long)g_nearScatterNear, g_near.state, (long long)g_nearScatterSeen, (long long)g_nearScatterMoved,
                (long long)g_nearScatterNoTile, (long long)g_nearScatterNoImage);
        }
    }
    if (made == 0 || out == NULL || NearTilesLive() == 0) return made;
    float cx = (float)x * (float)size, cy = (float)y * (float)size;
    if (NearTileNear(cx, cy, (float)size) < 0) return made;
    InterlockedIncrement64(&g_nearScatterNear);
    uintptr_t globals = 0, image = 0;
    if (SafeRead((uintptr_t)brush + 0x2858, &globals, 8) || globals == 0 || SafeRead(globals + 0x10, &image, 8) || image == 0) { InterlockedIncrement64(&g_nearScatterNoImage); return made; }
    uint8_t* inst = (uint8_t*)out;
    for (uint32_t i = 0; i < made && i < max; i++, inst += 0x30)
    {
        float p[3];
        memcpy(p, inst, 12);
        float tile = NearVisibleAt(p[0], p[1]);
        if (tile < -9000.0f) { InterlockedIncrement64(&g_nearScatterNoTile); continue; }
        float coarse = NearScatterImage(image, p[0], p[1]);
        if (coarse < -9000.0f) { InterlockedIncrement64(&g_nearScatterNoImage); continue; }
        p[2] += tile - coarse;
        memcpy(inst + 8, &p[2], 4);
        LONG64 n = InterlockedIncrement64(&g_nearScatterSeen);
        if (fabsf(tile - coarse) > 0.05f) InterlockedIncrement64(&g_nearScatterMoved);
        if (n == 1 || n == 100000 || n == 1000000)
            Note("difficulty: near terrain: scatter on the tile: %lld instances so far, %lld moved (this one %.2f m: tile %.2f, coarse %.2f at %.1f, %.1f)",
                (long long)n, (long long)g_nearScatterMoved, (double)(tile - coarse), (double)tile, (double)coarse, (double)p[0], (double)p[1]);
    }
    return made;
}

/* ---- 3D scatter past +-1024 m (grass, shrubs) ----------------------------------------------------------------------
 * The engine's scatter update (exe 0x212F10; the layer in rsi from 0x212F4A, its tree r13 = [layer+0x40] at 0x212F31)
 * gives each node in view ([tree+0x88], 0xE0 each, count [tree+0x90]; cell x node+0x54, y node+0x50) a box, min xyz at
 * node+0x58, max at +0x64 (0x2131E5/0x2131F0), from a per-layer table that op 0x97 (game 0x137BC90) builds for the
 * vanilla 2 km square only (-1024.0 at 0x137BCA7, 2048.0 at 0x137BCE6, texels x 2 + 0x800): a cell with an entry gets
 * xy = (index + 0.5) size +- (size 0.5 + [layer+0x230]) and z = entry+0x20 +- entry+0x24 (0x2133A2..0x2133C6); a cell
 * without one a point at z = 0 (0x213180), which the view test (0x9531E0, called only at 0x213226: rcx the tree, rdx
 * the frustum, r8d the stamp, xmm3 1.0) culls wherever the ground is not near 0, erasing its cached instances. On an
 * extra large map every node's box is rewritten before that test: the hit path's xy, and z the drawn ground's range
 * over the cell (and its pad), which is where the instances stand (NearScatterCell: the tile over its span, the
 * level's heights elsewhere), 1 m under and 3 m over. The call goes through a stub that keeps rsi (the layer) in a TLS
 * slot. Ranges are cached per cell (by size and index) for the level and the tile's state; only views of at most 1024
 * nodes with the tile in, and 256 new cells a call. */
#define NEAR_SCAT_CACHE (1u << 16)
typedef uint64_t (*NearViewFn)(uintptr_t tree, void* frustum, uint32_t stamp, float pad);
static struct
{
    DWORD tls; uint8_t* stub; int on;
    SRWLOCK lock; LONG gen;
    volatile LONG64 nodes, missed, calls; ULONGLONG noted; LONG notes, level;
} g_scatView = {.lock = SRWLOCK_INIT, .level = -1};
static struct { uint64_t key; float lo, hi; } g_scatCache[NEAR_SCAT_CACHE]; /* zero-filled: not in the file */

static float NearScatGround(float x, float y)
{
    float g = NearVisibleAt(x, y);
    return g > -9000.0f ? g : NearGroundAt(&g_nearCoarseGround, x, y);
}

/* The drawn ground's range over a cell (centre cx, cy; half width h); 0 when no ground is known there. */
static int NearScatRange(float size, int32_t ix, int32_t iy, float h, float* lo, float* hi, int* budget)
{
    LONG gen = (g_world.levels * 4 + (g_nearCoarseGround.v != NULL ? 2 : 0)) ^ (g_tilesGen << 12);
    uint32_t sbits; memcpy(&sbits, &size, 4);
    uint64_t key = ((uint64_t)sbits << 40) ^ ((uint64_t)(uint32_t)(ix & 0xFFFFF) << 20) ^ (uint64_t)(uint32_t)(iy & 0xFFFFF) ^ 0x8000000000000000ull;
    uint32_t slot = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 48) & (NEAR_SCAT_CACHE - 1);
    AcquireSRWLockShared(&g_scatView.lock);
    int fresh = g_scatView.gen == gen;
    for (uint32_t k = 0; fresh && k < 8; k++)
    {
        uint32_t at = (slot + k) & (NEAR_SCAT_CACHE - 1);
        if (g_scatCache[at].key == key) { *lo = g_scatCache[at].lo; *hi = g_scatCache[at].hi; ReleaseSRWLockShared(&g_scatView.lock); return 1; }
        if (g_scatCache[at].key == 0) break;
    }
    ReleaseSRWLockShared(&g_scatView.lock);
    if (g_nearCoarseGround.v == NULL || *budget <= 0) return 0;
    (*budget)--;
    float cx = ((float)ix + 0.5f) * size, cy = ((float)iy + 0.5f) * size, a = 1e9f, b = -1e9f;
    int n = (int)ceilf(2.0f * h / 2.0f); /* samples every 2 m at most */
    if (n < 2) n = 2;
    if (n > 64) n = 64;
    for (int j = 0; j <= n; j++)
        for (int i = 0; i <= n; i++)
        {
            float g = NearScatGround(cx - h + 2.0f * h * (float)i / (float)n, cy - h + 2.0f * h * (float)j / (float)n);
            if (g < -9000.0f) continue;
            if (g < a) a = g;
            if (g > b) b = g;
        }
    if (b < a) return 0;
    *lo = a; *hi = b;
    AcquireSRWLockExclusive(&g_scatView.lock);
    if (g_scatView.gen != gen) { memset(g_scatCache, 0, sizeof g_scatCache); g_scatView.gen = gen; }
    for (uint32_t k = 0; k < 8; k++)
    {
        uint32_t at = (slot + k) & (NEAR_SCAT_CACHE - 1);
        if (g_scatCache[at].key == 0 || g_scatCache[at].key == key || k == 7)
        { g_scatCache[at].key = key; g_scatCache[at].lo = a; g_scatCache[at].hi = b; break; }
    }
    ReleaseSRWLockExclusive(&g_scatView.lock);
    return 1;
}

static uint64_t NearScatterView(uintptr_t tree, void* frustum, uint32_t stamp, float pad)
{
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    uint8_t* layer = (uint8_t*)TlsGetValue(g_scatView.tls);
    if (layer != NULL && g_world.current > 1.0f && tree != 0 && *(uintptr_t*)(layer + 0x40) == tree)
    {
        float size = *(float*)(layer + 0x22C), grow = *(float*)(layer + 0x230);
        uint64_t count = *(uint64_t*)(tree + 0x90);
        uint8_t* nodes = *(uint8_t**)(tree + 0x88);
        int budget = 256; /* new cells' ranges a call; the rest come on later frames (cached) */
        /* the player's view only (about 170 nodes; the generator's whole-world sweep at load asks millions) and only
         * with the tile in (before it, NearVisibleAt has no tile) */
        if (nodes != NULL && count <= 1024 && NearTilesLive() > 0 && size >= 1.0f && size <= 4096.0f && grow >= 0.0f && grow <= 1024.0f)
        {
            float h = size * 0.5f + grow;
            uint32_t missed = 0;
            for (uint64_t i = 0; i < count; i++)
            {
                uint8_t* node = nodes + i * 0xE0;
                int32_t iy = *(int32_t*)(node + 0x50), ix = *(int32_t*)(node + 0x54);
                float lo, hi;
                if (!NearScatRange(size, ix, iy, h, &lo, &hi, &budget)) continue;
                float* box = (float*)(node + 0x58);
                if (box[0] == box[3] && box[2] == 0.0f && box[5] == 0.0f) missed++; /* the miss path's point */
                float cx = ((float)ix + 0.5f) * size, cy = ((float)iy + 0.5f) * size;
                box[0] = cx - h; box[1] = cy - h; box[2] = lo - 1.0f;
                box[3] = cx + h; box[4] = cy + h; box[5] = hi + 3.0f;
            }
            InterlockedAdd64(&g_scatView.nodes, (LONG64)count); InterlockedAdd64(&g_scatView.missed, missed); InterlockedIncrement64(&g_scatView.calls);
            ULONGLONG now = GetTickCount64();
            if (g_scatView.level != g_world.levels) { g_scatView.level = g_world.levels; g_scatView.notes = 0; g_scatView.noted = now; }
            if (now - g_scatView.noted > 20000 && g_scatView.notes < 6)
            {
                g_scatView.noted = now; g_scatView.notes++;
                Note("difficulty: near terrain: scatter boxes: %lld updates, %lld nodes given the ground's range, %lld of them had no table entry (a point at z 0)",
                    (long long)g_scatView.calls, (long long)g_scatView.nodes, (long long)g_scatView.missed);
            }
        }
    }
    return ((NearViewFn)(exe + 0x9531E0u))(tree, frustum, stamp, pad);
}

static void NearScatterViewInstall(void)
{
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    if (g_scatView.on) return;
    g_scatView.on = -1;
    if (!ApiCodeMatches(exe + 0x213226u, "E8 B5 FF 73 00 0F 28 B4 24 10 35 00 00 84 C0") || !ApiCodeMatches(exe + 0x21321Cu, "48 8D 55 F0 45 8B C6 49 8B CD") ||
        !ApiCodeMatches(exe + 0x212F31u, "4D 8B 69 40") || !ApiCodeMatches(exe + 0x212F4Au, "49 8B F1") ||
        !ApiCodeMatches(exe + 0x213090u, "F3 0F 10 9E 2C 02 00 00") || !ApiCodeMatches(exe + 0x2133ADu, "F3 0F 58 9E 30 02 00 00") ||
        !ApiCodeMatches(exe + 0x213066u, "49 39 9D 90 00 00 00") || !ApiCodeMatches(exe + 0x21309Fu, "4D 03 9D 88 00 00 00 49 63 53 50 41 8B 4B 54") ||
        !ApiCodeMatches(exe + 0x2131E5u, "41 0F 11 43 58") || !ApiCodeMatches(exe + 0x2131F0u, "F2 41 0F 11 43 68"))
    { Note("difficulty: near terrain: the scatter update is not the reviewed code; scatter keeps the vanilla square"); return; }
    g_scatView.tls = TlsAlloc();
    if (g_scatView.tls == TLS_OUT_OF_INDEXES || g_scatView.tls >= 64) { Note("difficulty: near terrain: no TLS slot for the scatter boxes"); return; }
    uint8_t* stub = AllocateNearGame(exe, 0x1000);
    if (stub == NULL) { Note("difficulty: near terrain: no room near the exe for the scatter stub"); return; }
    intptr_t reach = (intptr_t)stub - (intptr_t)(exe + 0x213226u + 5);
    if (reach != (intptr_t)(int32_t)reach) { Note("difficulty: near terrain: the scatter stub is out of reach"); return; }
    /* mov gs:[0x1480 + tls*8], rsi (the TEB's TlsSlots: TlsGetValue's); jmp [rip+0] NearScatterView */
    uint32_t slot = 0x1480u + g_scatView.tls * 8u;
    uint8_t code[23] = {0x65, 0x48, 0x89, 0x34, 0x25, 0, 0, 0, 0, 0xFF, 0x25, 0, 0, 0, 0};
    uint64_t to = (uint64_t)(uintptr_t)NearScatterView;
    memcpy(code + 5, &slot, 4); memcpy(code + 15, &to, 8);
    memcpy(stub, code, sizeof code);
    DWORD old = 0;
    if (!VirtualProtect(stub, 0x1000, PAGE_EXECUTE_READ, &old)) { Note("difficulty: near terrain: the scatter stub is not executable"); return; }
    uintptr_t site = exe + 0x213226u;
    if (!VirtualProtect((void*)site, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    *(volatile int32_t*)(site + 1) = (int32_t)reach;
    VirtualProtect((void*)site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, 5);
    g_scatView.stub = stub; g_scatView.on = 1;
    Note("difficulty: near terrain: scatter boxes: the view test's call goes through the ground's range (TLS slot %lu)", (unsigned long)g_scatView.tls);
}

/* ---- The render-side scatter's per-owner cell bitsets on a larger world ------------------------------------------------
 * Static (scratch crash215/, KB engine/crash-scatter-215b36): the render thread's ScatterSystem (message handler exe
 * 0x2144E0, render object type 17) keeps, per scattered resource (owner, key K = [resource+0xD0], map +0x138 {owner,
 * refcount}), a bitset of the 64 m cells ([scatter+0x1F8] = 64.0, exe 0x210592) holding its instances: width w =
 * 2 x (2048.0 / 64) (exe 0x1D429A, stored in the owner at +0xA4; bitset +0x28, dwords +0x20), bit (x + w/2) w + y. The
 * set (0x1D49A0) is bounds-checked, the clears (inline in messages 3 and 4, 0x215B36 / 0x215F34, and 0x1D4A00) are not:
 * on a 16 km world every remove of an instance past +-2048 m cleared a bit outside the bitset (up to 512 MB away). The
 * three readers of that 2048.0 for this width (0x1D3730, 0x1D429A, 0x1E789B; exe 0x16A9E1C) read the loader's value,
 * 2048 x the configured world scale, fixed for the process (each owner keeps the w it was made with, and every set,
 * clear and walk reads the owner's +0xA4: 0x1D450D, 0x1D4C33, 0x1D5E11, 0x1D65E1), so the bitset covers the world with
 * vanilla's margin (32 KB an owner at 8x). And a remove or re-own whose owner is missing (the find-or-insert 0x21D040
 * inserted it with a null owner; nothing in the engine checks) no longer reads through null: the entry's count is set to
 * 1 and vanilla's keep-the-bit path runs, which counts it down to 0 and erases it (0x215B72 / 0x215F74), so the maps
 * stay consistent; the misses are counted and noted. */
static struct { int on; float* extent; volatile LONG* missRemove; volatile LONG* missReown; LONG notedRemove, notedReown; } g_scatOwner;

static void ScatterOwnerNote(void)
{
    if (g_scatOwner.on != 1) return;
    LONG r = *g_scatOwner.missRemove, o = *g_scatOwner.missReown;
    if (r != g_scatOwner.notedRemove || o != g_scatOwner.notedReown)
    {
        g_scatOwner.notedRemove = r; g_scatOwner.notedReown = o;
        Note("difficulty: scatter: instances removed or re-owned with no owner entry (skipped, as a no-op): %ld removes, %ld re-owns so far", (long)r, (long)o);
    }
}

static void ScatterOwnerInstall(void)
{
    if (g_scatOwner.on) return;
    g_scatOwner.on = -1;
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    static const uint32_t reads[3] = {0x1D3730u, 0x1D429Au, 0x1E789Bu};
    for (int k = 0; k < 3; k++)
    {
        int32_t rel = 0;
        if (!ApiCodeMatches(exe + reads[k], "F3 0F 10 05") || SafeRead(exe + reads[k] + 4, &rel, 4) || exe + reads[k] + 8 + (intptr_t)rel != exe + 0x16A9E1Cu)
        { Note("difficulty: scatter: the owner bitset's width is not the reviewed code; its clears stay unbounded past +-2048 m"); return; }
    }
    if (!ApiCodeMatches(exe + 0x215B0Du, "48 89 44 24 20 40 84 FF 75 5B") || !ApiCodeMatches(exe + 0x215F24u, "4C 8B D8 40 84 FF 75 48") ||
        !ApiCodeMatches(exe + 0x215B72u, "4C 8D 5B 08 48 8B F8") || !ApiCodeMatches(exe + 0x215F74u, "41 83 43 08 FF"))
    { Note("difficulty: scatter: the owner lookups are not the reviewed code; a missing owner is not guarded"); return; }
    float wide = 2048.0f * (float)WorldScaleFor(g_difficulty.mapScale);
    if (wide < 2048.0f) wide = 2048.0f;
    uint8_t* page = AllocateNearGame(exe, 0x1000);
    if (page == NULL) { Note("difficulty: scatter: no room near the exe for the owner stubs"); return; }
    g_scatOwner.extent = (float*)page; *g_scatOwner.extent = wide;
    g_scatOwner.missRemove = (volatile LONG*)(page + 8); g_scatOwner.missReown = (volatile LONG*)(page + 12);
    *g_scatOwner.missRemove = 0; *g_scatOwner.missReown = 0;
    /* the two stubs: [mov], cmp qword [rax], 0; jne ok; lock inc [count]; mov dword [rax+8], 1; jmp keep; ok: test dil, dil;
     * jne keep; jmp clear */
    uint8_t* stubs[2] = {page + 0x40, page + 0x80};
    static const uint8_t movs[2][5] = {{0x48, 0x89, 0x44, 0x24, 0x20}, {0x4C, 0x8B, 0xD8, 0x90, 0x90}};
    const int movLen[2] = {5, 3};
    const uintptr_t keep[2] = {exe + 0x215B72u, exe + 0x215F74u}, clear[2] = {exe + 0x215B17u, exe + 0x215F2Cu};
    volatile LONG* counts[2] = {g_scatOwner.missRemove, g_scatOwner.missReown};
    for (int k = 0; k < 2; k++)
    {
        uint8_t* s = stubs[k]; int n = 0;
        memcpy(s + n, movs[k], (size_t)movLen[k]); n += movLen[k];
        static const uint8_t cmp[6] = {0x48, 0x83, 0x38, 0x00, 0x75, 0x13};
        memcpy(s + n, cmp, 6); n += 6;
        s[n++] = 0xF0; s[n++] = 0xFF; s[n++] = 0x05;
        int32_t rel = (int32_t)((intptr_t)counts[k] - (intptr_t)(s + n + 4)); memcpy(s + n, &rel, 4); n += 4;
        static const uint8_t one[7] = {0xC7, 0x40, 0x08, 0x01, 0x00, 0x00, 0x00};
        memcpy(s + n, one, 7); n += 7;
        s[n++] = 0xE9; rel = (int32_t)((intptr_t)keep[k] - (intptr_t)(s + n + 4)); memcpy(s + n, &rel, 4); n += 4;
        s[n++] = 0x40; s[n++] = 0x84; s[n++] = 0xFF;
        s[n++] = 0x0F; s[n++] = 0x85; rel = (int32_t)((intptr_t)keep[k] - (intptr_t)(s + n + 4)); memcpy(s + n, &rel, 4); n += 4;
        s[n++] = 0xE9; rel = (int32_t)((intptr_t)clear[k] - (intptr_t)(s + n + 4)); memcpy(s + n, &rel, 4); n += 4;
    }
    DWORD old = 0;
    if (!VirtualProtect(page, 0x1000, PAGE_EXECUTE_READWRITE, &old)) { Note("difficulty: scatter: the owner stubs are not executable"); return; }
    /* the three width reads point at the loader's extent */
    for (int k = 0; k < 3; k++)
    {
        uintptr_t at = exe + reads[k];
        intptr_t d = (intptr_t)g_scatOwner.extent - (intptr_t)(at + 8);
        if (d != (intptr_t)(int32_t)d) { Note("difficulty: scatter: the extent is out of reach"); return; }
        int32_t rel = (int32_t)d;
        if (ApiWrite(at + 4, &rel, 4)) { Note("difficulty: scatter: the owner bitset's width could not be redirected"); return; }
    }
    const uintptr_t sites[2] = {exe + 0x215B0Du, exe + 0x215F24u};
    const int lens[2] = {10, 8};
    for (int k = 0; k < 2; k++)
    {
        uint8_t jump[10] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
        intptr_t d = (intptr_t)stubs[k] - (intptr_t)(sites[k] + 5);
        if (d != (intptr_t)(int32_t)d) { Note("difficulty: scatter: the owner stubs are out of reach"); return; }
        int32_t rel = (int32_t)d; memcpy(jump + 1, &rel, 4);
        if (ApiWrite(sites[k], jump, (size_t)lens[k])) { Note("difficulty: scatter: an owner lookup could not be guarded"); return; }
    }
    g_scatOwner.on = 1;
    Note("difficulty: scatter: owner cell bitsets cover +-%.0f m (vanilla +-2048 m); removes and re-owns with no owner entry are skipped", (double)wide);
}

static void TerrainInstall(uintptr_t game)
{
    if (g_terrainNoise != NULL) return;
    ScatterOwnerInstall();
    MoveInstall(game);
    NearBoundsInstall();
    SceneSwapInstall(); /* after it: the same lock calls */
    if (!ApiCodeMatches(game + 0xF3A2E0u, "48 8B C4 48 89 58 18 48 89 70 20 F3 0F 11 48 10 57 48 81 EC"))
    {
        Note("difficulty: terrain: the generator's samplers are not the reviewed ones; extra large maps keep the level's own terrain");
        return;
    }
    if (LoaderHook("extra large maps' terrain noise", game + 0xF3A2E0u, (void*)TerrainNoise, (void**)&g_terrainNoise))
        Note("difficulty: terrain: a sampler could not be hooked; extra large maps keep the level's own terrain");
    if (!ApiCodeMatches(game + 0x137A2A0u, "40 55 53 56 57 41 54 41 56 41 57 48 8D AC 24 E0 FE FF FF") ||
        !ApiCodeMatches(game + 0x137A9B9u, "F2 0F 11 06") || !ApiCodeMatches(game + 0x137A9DDu, "89 46 08") ||
        LoaderHook("extra large maps' scatter on the near tile", game + 0x137A2A0u, (void*)NearScatterCell, (void**)&g_nearScatter))
        Note("difficulty: near terrain: the scatter callback is not the reviewed one; scatter keeps the coarse ground under the tile");
    char off[4] = "";
    if (!(GetEnvironmentVariableA("HELLFORGE_XL_SCATTER_BOXES", off, sizeof off) > 0 && off[0] == '0')) NearScatterViewInstall();
}
