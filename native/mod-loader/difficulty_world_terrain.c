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
 * (0xF3A2E0, plateau outlines) and the height texture GenerateSubregionHeights adds (0xF64B80). While a tile builds,
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
typedef float (*TerrainTextureFn)(void* texture, uint64_t position);
static TerrainNoiseFn g_terrainNoise;
static TerrainTextureFn g_terrainTexture;
static volatile DWORD g_terrainThread;   /* the thread building a tile (the samplers' offset applies there only) */
static float g_terrainOffset[2];

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
    uint8_t record[0x19B0];
    uint8_t phase;
    double buildMs, drawMs;
    uint32_t frames;
    ULONGLONG started;
} g_terrain = {.level = -1};

/* The fBm noise (height groups' plateau outlines, GenerateSubregionHeights): world positions for a tile. */
static float TerrainNoise(void* noise, float x, float y)
{
    if (g_terrainThread != 0 && g_terrainThread == GetCurrentThreadId()) { x += g_terrainOffset[0]; y += g_terrainOffset[1]; }
    return g_terrainNoise(noise, x, y);
}

/* The height texture GenerateSubregionHeights adds (position x the world's inverse size, which an extra large world
 * redirects to 1 / its size): for a tile, the world position wrapped to a vanilla world and scaled so the redirected
 * constant samples it as vanilla does. */
static float TerrainTexture(void* texture, uint64_t position)
{
    if (g_terrainThread != 0 && g_terrainThread == GetCurrentThreadId() && g_world.values != NULL)
    {
        float p[2];
        memcpy(p, &position, 8);
        float scale = g_world.values->full / 2048.0f;
        for (int k = 0; k < 2; k++)
        {
            float w = fmodf(p[k] + g_terrainOffset[k], 2048.0f);
            if (w < 0) w += 2048.0f;
            p[k] = w * scale;
        }
        memcpy(&position, p, 8);
    }
    return g_terrainTexture(texture, position);
}

static int TerrainOn(void)
{
    static int on = -1;
    if (on < 0) { char t[8] = ""; on = !(GetEnvironmentVariableA("HELLFORGE_XL_TERRAIN", t, sizeof t) > 0 && t[0] == '0'); }
    return on && g_terrainNoise != NULL && g_terrainTexture != NULL;
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
    memset(g_terrain.tmpl + 0xEE10C8u, 0, 0x10B1F48u - 0xEE10C8u);
    return 1;
}

/* One tile: the game's GenerateVoronoi and height passes on the tile's block, then moved to its world position.
 * 1 when built. */
static int TerrainBuild(int tile, uint64_t records, uint32_t end, uint64_t context, uint64_t g, uint64_t h, uintptr_t gen)
{
    const float extent = (float)TERRAIN_TILE_EXTENT, core = (float)TERRAIN_TILE_CORE;
    float cx = (float)(tile % g_terrain.columns) * core, cy = (float)(tile / g_terrain.columns) * core;
    float ox = cx - (float)TERRAIN_TILE_MARGIN, oy = cy - (float)TERRAIN_TILE_MARGIN;
    uintptr_t settings = 0, lg = 0, p = 0, spacingAt = 0;
    uint32_t disk = 0;
    if (SafeRead(context + 8, &settings, 8) || settings == 0 || SafeRead(context + 0x18, &lg, 8) || lg == 0 ||
        SafeRead(lg + 0x2D0, &p, 8) || p == 0 || SafeRead(p + 0x68, &spacingAt, 8) || spacingAt == 0 || SafeRead(lg + 0x2D7C, &disk, 4))
        return 0;
    memcpy(g_terrain.block, g_terrain.tmpl, GEN_BLOCK_SIZE);
    uint64_t seed = TerrainSeed(tile);
    static uint8_t one[0x19B0];
    if (SafeRead(g_rec25, one, sizeof one)) return 0;
    WorldTileRelocate(one, gen, (uintptr_t)g_terrain.block);
    { uint64_t r; memcpy(&r, one + 0x1990, 8); r ^= seed; memcpy(one + 0x1990, &r, 8); }

    volatile uint32_t* sizeP = (volatile uint32_t*)(settings + 0x2C);
    volatile float* loP = (volatile float*)(settings + 0x64);
    volatile float* hiP = (volatile float*)(settings + 0x68);
    volatile float* radiusP = (volatile float*)(settings + 0x6C);
    volatile float* spacingP = (volatile float*)spacingAt;
    volatile uint32_t* diskP = (volatile uint32_t*)(lg + 0x2D7C);
    uint32_t size = *sizeP;
    float lo = *loP, hi = *hiP, radius = *radiusP, spacing = *spacingP;
    *diskP = disk ^ 1u;
    *sizeP = (uint32_t)extent; *loP = 0.0f; *hiP = extent; *radiusP = extent; *spacingP = 16.0f;
    g_terrainOffset[0] = ox; g_terrainOffset[1] = oy;
    g_terrainThread = GetCurrentThreadId();

    typedef void (*Handler)(uint64_t, uint64_t, uint64_t);
    ((Handler)(g_world.game + 0xF3F8E0u))((uint64_t)(uintptr_t)one + 8, (uint64_t)(uintptr_t)one, context);
    int ok = 1, seen04 = 0;
    uint32_t start = 0xFFFFFFFFu, op = 0;
    for (uint32_t i = 0; i < end; i++)
        if (!SafeRead(records + (uint64_t)i * 0x19B0u, &op, 4) && op == 0x25) { start = i + 1; break; }
    if (start == 0xFFFFFFFFu) ok = 0;
    for (uint32_t i = start; ok && i < end; i++)
    {
        if (SafeRead(records + (uint64_t)i * 0x19B0u, one, sizeof one)) { ok = 0; break; }
        memcpy(&op, one, 4);
        if (!WorldTileRuns(op)) continue;
        WorldTileRelocate(one, gen, (uintptr_t)g_terrain.block);
        { uint64_t r; memcpy(&r, one + 0x1990, 8); r ^= seed; memcpy(one + 0x1990, &r, 8); }
        volatile uint32_t index = 0;
        volatile int calls = 0;
        uint8_t phase = 0;
        LONG fault = WorldTileGuarded(one, context, &index, &phase, g, h, 10000, &calls);
        if (fault || index == 0)
        {
            Note("difficulty: terrain: tile %d (%.0f, %.0f) %s in op 0x%02X%s", tile, (double)ox, (double)oy, fault ? "faulted" : "did not finish", op,
                fault ? "; the level's own cells stay there" : "");
            ok = 0;
            break;
        }
        if (op == 0x04) seen04 = 1;
        if (op == 0x2D && seen04) break;
    }
    g_terrainThread = 0;
    *sizeP = size; *loP = lo; *hiP = hi; *radiusP = radius; *spacingP = spacing; *diskP = disk;
    if (!ok || !seen04) return 0;

    /* to the tile's world position: the Voronoi vertices (x, y at +0) and the route-cell areas' points (x, y at +0x80)
     * the route cells name ([+0xA0, +0xA4)) */
    uint8_t* T = g_terrain.block;
    uint32_t vertices = 0, cells = 0, last = 0;
    memcpy(&vertices, T + 0x11A7090, 4);
    memcpy(&cells, T + 0x11A71C0, 4);
    if (vertices > 6000 || cells > 4096) return 0;
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
    static uint8_t* moved;
    uint32_t blockSize = GEN_BLOCK_SIZE;
    if (moved == NULL) moved = (uint8_t*)VirtualAlloc(NULL, blockSize / 64 + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
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
    /* Only the core is drawn: a cell whose centre (its edges' ends, as the render finds it) lies outside loses its
     * edges, which the render skips; a route-cell area point outside loses its segments. Neighbouring tiles meet on
     * cells built well inside both (gaps keep the level's own cells, drawn first). */
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
        if (sx < cx || sx >= cx + core || sy < cy || sy >= cy + core) memset(cell + 8, 0, 16 * 8);
    }
    for (uint32_t k = 0; k < last; k++)
    {
        uint8_t* point = T + 0xEE10C8 + (size_t)k * 0x90;
        float xy[2];
        memcpy(xy, point + 0x80, 8);
        if (xy[0] < cx || xy[0] >= cx + core || xy[1] < cy || xy[1] >= cy + core) memset(point, 0, 16 * 8);
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

/* The dispatcher hook (difficulty_world.c's WorldDispatch) at the level's SmoothHeightmap: 1 while the level waits. */
static int TerrainStep(uint64_t records, uint32_t end, uint32_t at, uint64_t context, uint64_t g, uint64_t h)
{
    if (g_world.current != 8.0f || g_worldLevel == 0 || g_rec25 == 0 || !TerrainOn()) return 0;
    uintptr_t gen = 0;
    if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0) return 0;
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
        Note("difficulty: terrain: building the map at vanilla density, %d m tiles (%d m drawn; %d x %d) into the level's merge (record %u); the level waits",
            TERRAIN_TILE_EXTENT, TERRAIN_TILE_CORE, g_terrain.columns, g_terrain.columns, g_terrain.draw);
    }
    if (g_terrain.state != 1) return 0;
    g_terrain.frames++;
    LARGE_INTEGER hz, t0, t1;
    QueryPerformanceFrequency(&hz);
    QueryPerformanceCounter(&t0);
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
                return 0;
            }
            LARGE_INTEGER b0, b1;
            QueryPerformanceCounter(&b0);
            int built = TerrainBuild(g_terrain.next, records, end, context, g, h, gen) && TerrainDrawPrepare(records, gen);
            QueryPerformanceCounter(&b1);
            g_terrain.buildMs += (double)(b1.QuadPart - b0.QuadPart) * 1000.0 / (double)hz.QuadPart;
            if (!built) { g_terrain.failed++; g_terrain.next++; continue; }
            g_terrain.building = 1;
        }
        LARGE_INTEGER d0, d1;
        QueryPerformanceCounter(&d0);
        volatile uint32_t index = 0;
        volatile int calls = 0;
        LONG fault = WorldTileGuarded(g_terrain.record, context, &index, &g_terrain.phase, g, h, 1, &calls);
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
        if (index != 0) { g_terrain.drawn++; g_terrain.building = 0; g_terrain.next++; }
    }
}

/* ---- Near-player terrain (first step: one 2 km terrain at the drop point) ----------------------------------------------
 * The capture (CaptureHeightmapFinalize 0xF53BA0) leaves the 4096^2 u16 heightmap readback in the terrain description
 * T ([[0x5E pass+0x30]] = [0x69 pass+0]): T+0xC0 columns/rows (u32 each), +0xC8 1, +0xD0 the samples. The engine copies
 * the heights into the terrain's own buffer (the heightfield exe+0x2F2CB0 builds from is not the readback). So a
 * second terrain spawned while T points at other samples should carry them. InitSpawnTerrain (0xF44DC0) spawns the
 * unit through [api+8]+0x198 with an identity pose at (0, 0, -500) and a size of the world's - 0.5 on both axes (heights
 * -500 + v / 65535 x 1000): the wrapper below moves and sizes it. Then the 0x69 wait ([api+8]+0x1A0), SpawnTerrainWork
 * ([api+8]+0x1A8) and SpawnTerrainFinalize (0xF451E0) as the level runs them. */
#define NEAR_SIDE 2048.0f
#define NEAR_SAMPLES 4096
typedef uint64_t (*NearSpawnFn)(void* world, float* pose, uint32_t a, uint32_t type, uint32_t name, uint64_t resource, uint32_t hash,
    float* size, float* range, uint8_t flag);
static NearSpawnFn g_nearSpawnOriginal;
static struct {
    LONG level; int state; uint32_t at6B, polls;
    uint8_t one69[0x19B0], one6B[0x19B0];
    uint64_t T, handle, handleCell, oldSamples;
    float* global; uint16_t* tile; uint32_t columns, rows;
    float centre[2];
    int ready; /* the drop point is chosen: the terrain goes in on the next dispatch */
} g_near = {.level = -1};

static uint64_t NearSpawn(void* world, float* pose, uint32_t a, uint32_t type, uint32_t name, uint64_t resource, uint32_t hash, float* size,
    float* range, uint8_t flag)
{
    if (type == 0x2A9E3232u && name == 0x95E7E3ACu && g_terrainThread == GetCurrentThreadId())
    {
        pose[12] += g_near.centre[0]; pose[13] += g_near.centre[1];
        size[0] = NEAR_SIDE - 0.5f; size[1] = NEAR_SIDE - 0.5f;
    }
    return g_nearSpawnOriginal(world, pose, a, type, name, resource, hash, size, range, flag);
}

/* The tile's samples: the level's heightmap under it (bilinear, 8 samples per level sample), lifted 2 m, with 24 m
 * ridges 1.5 m high: a test pattern that shows whether the second terrain carries its own heights. */
static void NearFill(int orientation)
{
    float scale = 1000.0f / 65535.0f;
    float step = NEAR_SIDE / NEAR_SAMPLES, gstep = 16384.0f / (float)g_near.columns;
    for (int r = 0; r < NEAR_SAMPLES; r++)
        for (int c = 0; c < NEAR_SAMPLES; c++)
        {
            float wx = g_near.centre[0] - NEAR_SIDE / 2 + (c + 0.5f) * step, wy = g_near.centre[1] - NEAR_SIDE / 2 + (r + 0.5f) * step;
            float gx = (wx + 8192.0f) / gstep - 0.5f, gy = (wy + 8192.0f) / gstep - 0.5f;
            if (orientation & 1) gx = (float)g_near.columns - 1 - gx;
            if (orientation & 2) gy = (float)g_near.rows - 1 - gy;
            int x0 = (int)floorf(gx), y0 = (int)floorf(gy);
            float fx = gx - x0, fy = gy - y0;
            if (x0 < 0) { x0 = 0; fx = 0; } if (y0 < 0) { y0 = 0; fy = 0; }
            if (x0 > (int)g_near.columns - 2) { x0 = g_near.columns - 2; fx = 1; } if (y0 > (int)g_near.rows - 2) { y0 = g_near.rows - 2; fy = 1; }
            const float* g = g_near.global + (size_t)y0 * g_near.columns + x0;
            float h = (g[0] * (1 - fx) + g[1] * fx) * (1 - fy) + (g[g_near.columns] * (1 - fx) + g[g_near.columns + 1] * fx) * fy;
            float m = h + 2.0f + 0.75f * (sinf(wx * 0.2618f) + sinf(wy * 0.2618f));
            float v = (m + 500.0f) / scale;
            if (v < 0) v = 0; if (v > 65535) v = 65535;
            /* the tile's array in the readback's orientation */
            int tc = (orientation & 1) ? NEAR_SAMPLES - 1 - c : c, tr = (orientation & 2) ? NEAR_SAMPLES - 1 - r : r;
            g_near.tile[(size_t)tr * NEAR_SAMPLES + tc] = (uint16_t)(v + 0.5f);
        }
}

/* Research: a terrain's engine entry (the table world+0x230550, looked up as SpawnTerrainWork does with exe+0x1B86C0, a
 * find-or-insert: only for handles that were spawned): origin/extent, sample grid, the render object's origin/extent and
 * its heights. */
static void NearNoteEntry(const char* what, uint64_t world, uint64_t handle)
{
    typedef uint8_t* (*FindFn)(uint64_t table, uint64_t* key);
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    uint64_t key = handle;
    uint8_t* e = ((FindFn)(exe + 0x1B86C0u))(world + 0x230550u, &key);
    if (e == NULL) { Note("difficulty: near terrain: %s: no entry", what); return; }
    float oe[4] = {0}, ro[4] = {0};
    uint64_t obj = 0, dimsAt = 0, heights = 0;
    uint32_t dims[2] = {0};
    uint16_t mid[2] = {0};
    SafeRead((uintptr_t)e + 0x24, oe, 16); SafeRead((uintptr_t)e + 0x10, &obj, 8); SafeRead((uintptr_t)e + 0x38, &dimsAt, 8);
    if (dimsAt) SafeRead(dimsAt + 0x14, dims, 8);
    if (obj) { SafeRead(obj + 0x168, ro, 16); SafeRead(obj + 0x40, &heights, 8); }
    if (heights && dims[0] && dims[1]) SafeRead(heights + ((uint64_t)dims[1] / 2 * dims[0] + dims[0] / 2) * 2, mid, 4);
    Note("difficulty: near terrain: %s: handle 0x%llX entry origin (%.1f, %.1f) extent (%.1f, %.1f), grid %u x %u, object origin (%.1f, %.1f) "
        "extent (%.1f, %.1f), heights 0x%llX, middle %u %u", what, (unsigned long long)handle, (double)oe[0], (double)oe[1], (double)oe[2], (double)oe[3],
        dims[0], dims[1], (double)ro[0], (double)ro[1], (double)ro[2], (double)ro[3], (unsigned long long)heights, mid[0], mid[1]);
}

static int NearOn(void)
{
    static int on = -1;
    if (on < 0) { char text[8] = ""; on = !(GetEnvironmentVariableA("HELLFORGE_XL_NEAR", text, sizeof text) > 0 && text[0] == '0'); }
    return on;
}

/* The level's heightmap at a world point, under one of the four axis flips (metres). */
static float NearGlobalAt(float wx, float wy, int orientation)
{
    float gstep = 16384.0f / (float)g_near.columns, shift = 8192.0f;
    if (orientation & 4) { float t = wx; wx = wy; wy = t; } /* axes swapped */
    int c = (int)((wx + shift) / gstep), r = (int)((wy + shift) / gstep);
    if (orientation & 1) c = (int)g_near.columns - 1 - c;
    if (orientation & 2) r = (int)g_near.rows - 1 - r;
    if (c < 0 || r < 0 || c >= (int)g_near.columns || r >= (int)g_near.rows) return -9999.0f;
    return g_near.global[(size_t)r * g_near.columns + c];
}

/* The first player's position against the level's heightmap under each flip (which flip the terrain uses), every 5 s
 * for a minute once the near terrain's level is in. */
static void NearReportPlayer(const float* at)
{
    static ULONGLONG last; static int reports; static LONG level = -1;
    if (g_near.global == NULL || g_near.level != g_world.levels || g_world.current != 8.0f) return;
    if (level != g_near.level) { level = g_near.level; reports = 0; last = 0; }
    ULONGLONG now = GetTickCount64();
    if (reports >= 40 || now - last < 1500) return;
    last = now; reports++;
    Note("difficulty: near terrain: player at (%.1f, %.1f, %.2f); the level's heightmap there by flip: %.2f %.2f %.2f %.2f; axes swapped: %.2f %.2f %.2f %.2f",
        (double)at[0], (double)at[1], (double)at[2], (double)NearGlobalAt(at[0], at[1], 0), (double)NearGlobalAt(at[0], at[1], 1),
        (double)NearGlobalAt(at[0], at[1], 2), (double)NearGlobalAt(at[0], at[1], 3), (double)NearGlobalAt(at[0], at[1], 4),
        (double)NearGlobalAt(at[0], at[1], 5), (double)NearGlobalAt(at[0], at[1], 6), (double)NearGlobalAt(at[0], at[1], 7));
}

/* ---- The drop point (research runs; the harness reads xl-drop.txt) ----------------------------------------------------
 * Water: GenerateWaterStart (op 0x0A, 0xF6C610) walks [[pass+0x58]] ids (u32, [pass+0x18]) into the area table
 * ([pass+0x10], pointers) and spawns a water body for each area whose level [area+0xE4] is above -500. The drop goes on
 * dry, gentle ground of the final heightmap (CaptureHeightmapFinalize's float32 readback, row 0 at +y): 3 m above the
 * highest water, slope under 12 deg over +-16 m, under 20 m of relief within +-64 m, inside 1200 m of the centre. */
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
    TerrainNearDrop();
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
    if (g_near.level == g_world.levels && g_near.state == 1) { g_near.centre[0] = bestX; g_near.centre[1] = bestY; g_near.ready = 1; }
    Note("difficulty: near terrain: drop point (%.0f, %.0f), ground %.2f m (water up to %.2f m), %d dry gentle candidates", (double)bestX, (double)bestY,
        (double)NearHeight((int)g_near.rows - 1 - (int)((bestY + 8192.0f) / step), (int)((bestX + 8192.0f) / step)), (double)g_nearWater.high, candidates);
}

static int TerrainNearStep(uint64_t records, uint32_t end, uint32_t at, uint32_t op, uint64_t context)
{
    if (!TerrainOn() || g_world.current != 8.0f) return 0;
    if (op == 0x69 && g_near.level != g_world.levels)
    {
        g_near.level = g_world.levels; g_near.state = 0;
        uint64_t record = records + (uint64_t)at * 0x19B0u, T = 0;
        uint32_t dims[2] = {0}; uint64_t samples = 0;
        if (SafeRead(record, g_near.one69, sizeof g_near.one69)) { Note("difficulty: near terrain: the spawn record did not read"); return 0; }
        memcpy(&T, g_near.one69 + 8, 8);
        if (T == 0 || SafeRead(T + 0xC0, dims, 8) || SafeRead(T + 0xD0, &samples, 8) || samples == 0 || dims[0] != 4096 || dims[1] != 4096)
        { Note("difficulty: near terrain: the description is not the reviewed one (T 0x%llX, %u x %u)", (unsigned long long)T, dims[0], dims[1]); return 0; }
        size_t bytes = (size_t)dims[0] * dims[1] * 4; /* float32 metres */
        if (g_near.global == NULL) g_near.global = (float*)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (g_near.tile == NULL) g_near.tile = (uint16_t*)VirtualAlloc(NULL, (size_t)NEAR_SAMPLES * NEAR_SAMPLES * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (g_near.global == NULL || g_near.tile == NULL || SafeRead(samples, g_near.global, bytes)) { Note("difficulty: near terrain: the level's heightmap at 0x%llX did not copy", (unsigned long long)samples); return 0; }
        g_near.T = T; g_near.columns = dims[0]; g_near.rows = dims[1];
        uint32_t o = 0; g_near.at6B = 0;
        for (uint32_t i = at + 1; i < end && i < at + 16; i++) if (!SafeRead(records + (uint64_t)i * 0x19B0u, &o, 4) && o == 0x6B) { g_near.at6B = i; break; }
        if (g_near.at6B == 0 || SafeRead(records + (uint64_t)g_near.at6B * 0x19B0u, g_near.one6B, sizeof g_near.one6B)) { Note("difficulty: near terrain: no SpawnTerrainFinalize after record %u", at); return 0; }
        Note("difficulty: near terrain: level heightmap copied; the terrain goes in after record %u", g_near.at6B);
        g_near.state = 1; g_near.ready = 0;
        TerrainNearDrop();
        return 0;
    }
    if (g_near.level != g_world.levels || at <= g_near.at6B) return 0;
    if (g_near.state == 1 && g_near.ready)
    {
        g_near.state = 0;
        if (!NearOn()) { Note("difficulty: near terrain: off (HELLFORGE_XL_NEAR=0)"); return 0; }
        uintptr_t gen = 0;
        if (g_worldLevel == 0 || SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0) { Note("difficulty: near terrain: no generator block"); return 0; }
        (void)gen;
        int orientation = 2; /* measured (i14): centred, rows from +y down */
        NearFill(orientation);
        uintptr_t api = 0, table = 0;
        if (SafeRead(g_world.game + 0x3326308u, &api, 8) || api == 0 || SafeRead(api + 8, &table, 8) || table == 0) { Note("difficulty: near terrain: no engine table"); return 0; }
        volatile uint64_t* slot = (volatile uint64_t*)(table + 0x198);
        DWORD old = 0;
        if (!VirtualProtect((void*)slot, 8, PAGE_READWRITE, &old)) { Note("difficulty: near terrain: the spawn slot is not writable"); return 0; }
        g_nearSpawnOriginal = (NearSpawnFn)(uintptr_t)*slot;
        if (SafeRead(g_near.T + 0xD0, &g_near.oldSamples, 8)) return 0;
        *(volatile uint64_t*)(uintptr_t)(g_near.T + 0xD0) = (uint64_t)(uintptr_t)g_near.tile;
        *slot = (uint64_t)(uintptr_t)NearSpawn;
        g_terrainThread = GetCurrentThreadId();
        memset(g_near.one69 + 8 + 0x60, 0, 8);
        typedef void (*InitFn)(uint64_t pass, uint64_t unused, uint64_t context);
        ((InitFn)(g_world.game + 0xF44DC0u))((uint64_t)(uintptr_t)g_near.one69 + 8, 0, context);
        g_terrainThread = 0;
        *slot = (uint64_t)(uintptr_t)g_nearSpawnOriginal;
        VirtualProtect((void*)slot, 8, old, &old);
        memcpy(&g_near.handle, g_near.one69 + 8 + 0x60, 8);
        Note("difficulty: near terrain: spawned a %.0f m terrain at (%.0f, %.0f) (orientation %d), handle 0x%llX; waiting for it",
            (double)NEAR_SIDE, (double)g_near.centre[0], (double)g_near.centre[1], orientation, (unsigned long long)g_near.handle);
        if (g_near.handle == 0) { *(volatile uint64_t*)(uintptr_t)(g_near.T + 0xD0) = g_near.oldSamples; return 0; }
        g_near.state = 2; g_near.polls = 0;
        return 1;
    }
    if (g_near.state == 2)
    {
        uintptr_t api = 0, table = 0, world = 0;
        SafeRead(g_world.game + 0x3326308u, &api, 8); SafeRead(api + 8, &table, 8); SafeRead(context + 0x40, &world, 8);
        typedef char (*ReadyFn)(uint64_t world, uint64_t handle);
        typedef void (*WorkFn)(uint64_t world, uint64_t handle);
        ReadyFn ready = NULL; WorkFn work = NULL;
        SafeRead(table + 0x1A0, &ready, 8); SafeRead(table + 0x1A8, &work, 8);
        if (ready == NULL || work == NULL || world == 0) { g_near.state = 0; *(volatile uint64_t*)(uintptr_t)(g_near.T + 0xD0) = g_near.oldSamples; return 0; }
        if (!ready(world, g_near.handle))
        {
            if (++g_near.polls < 3000) return 1;
            Note("difficulty: near terrain: the terrain unit was not ready after %u polls; left as it is", g_near.polls);
            g_near.state = 0; *(volatile uint64_t*)(uintptr_t)(g_near.T + 0xD0) = g_near.oldSamples; return 0;
        }
        {   /* the terrain's own samples (it copied the level's whole map): the tile's go in before the work builds its
             * collision from them */
            typedef uint8_t* (*FindFn)(uint64_t table, uint64_t* key);
            uint64_t key = g_near.handle, obj = 0, dimsAt = 0, heights = 0;
            uint32_t dims[2] = {0};
            uint8_t* e = ((FindFn)((uintptr_t)GetModuleHandleW(NULL) + 0x1B86C0u))(world + 0x230550u, &key);
            if (e != NULL) { SafeRead((uintptr_t)e + 0x10, &obj, 8); SafeRead((uintptr_t)e + 0x38, &dimsAt, 8); }
            if (obj) SafeRead(obj + 0x40, &heights, 8);
            if (dimsAt) SafeRead(dimsAt + 0x14, dims, 8);
            if (heights == 0 || dims[0] != NEAR_SAMPLES || dims[1] != NEAR_SAMPLES)
                Note("difficulty: near terrain: the terrain's samples are not the expected %d^2 (%u x %u at 0x%llX); left as copied", NEAR_SAMPLES, dims[0], dims[1],
                    (unsigned long long)heights);
            else memcpy((void*)(uintptr_t)heights, g_near.tile, (size_t)NEAR_SAMPLES * NEAR_SAMPLES * 2);
        }
        work(world, g_near.handle);
        g_near.handleCell = g_near.handle;
        uint64_t cell = (uint64_t)(uintptr_t)&g_near.handleCell;
        memcpy(g_near.one6B + 8, &cell, 8);
        typedef char (*FinalizeFn)(uint64_t pass, uint64_t unused, uint64_t context);
        ((FinalizeFn)(g_world.game + 0xF451E0u))((uint64_t)(uintptr_t)g_near.one6B + 8, 0, context);
        *(volatile uint64_t*)(uintptr_t)(g_near.T + 0xD0) = g_near.oldSamples;
        Note("difficulty: near terrain: the %.0f m terrain is in (%u polls)", (double)NEAR_SIDE, g_near.polls);
        {
            uint64_t levelHandle = 0;
            SafeRead(g_near.T - 8, &levelHandle, 8);
            if (levelHandle) NearNoteEntry("the level's terrain", world, levelHandle);
            NearNoteEntry("the near terrain", world, g_near.handle);
            uint16_t t[2] = {g_near.tile[(size_t)2048 * NEAR_SAMPLES + 2048], g_near.tile[(size_t)2048 * NEAR_SAMPLES + 2049]};
            Note("difficulty: near terrain: its samples at 0x%llX, middle %u %u", (unsigned long long)(uintptr_t)g_near.tile, t[0], t[1]);
        }
        g_near.state = 3;
        return 0;
    }
    return 0;
}

/* The samplers' wrappers, on any larger map (they only act while a tile builds). */
static void TerrainInstall(uintptr_t game)
{
    if (g_terrainNoise != NULL) return;
    if (!ApiCodeMatches(game + 0xF3A2E0u, "48 8B C4 48 89 58 18 48 89 70 20 F3 0F 11 48 10 57 48 81 EC") ||
        !ApiCodeMatches(game + 0xF64B80u, "48 89 54 24 10 48 83 EC 28 F3 0F 10 44 24 38 4C 8D 44 24 38"))
    {
        Note("difficulty: terrain: the generator's samplers are not the reviewed ones; extra large maps keep the level's own terrain");
        return;
    }
    if (LoaderHook("extra large maps' terrain noise", game + 0xF3A2E0u, (void*)TerrainNoise, (void**)&g_terrainNoise) ||
        LoaderHook("extra large maps' terrain texture", game + 0xF64B80u, (void*)TerrainTexture, (void**)&g_terrainTexture))
        Note("difficulty: terrain: a sampler could not be hooked; extra large maps keep the level's own terrain");
}
