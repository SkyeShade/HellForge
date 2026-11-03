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
/* The height texture's CPU image, as the sampler ([api+0x10]+0x1F0 = exe 0x31C8B0) reads it: {w u32, h u32, format i32,
 * ?, data at +0x10}, texel bits from the exe's table at 0x1658990. It is copied the first time a tile samples it in a
 * level, and tiles sample the copy: the near-player tile builds tiles again at the level's CleanupTerrains (0x65), and
 * the engine releases such images' CPU data during generation (a tile at op 0x6D read a null image at exe+0x31CA66). */
static struct { uint64_t key; uint8_t desc[0x20]; uint8_t* data; } g_terrainImages[4];
static LONG g_terrainImagesLevel = -1;

static void* TerrainImage(void* texture)
{
    if (g_terrainImagesLevel != g_world.levels)
    {
        g_terrainImagesLevel = g_world.levels;
        for (int k = 0; k < 4; k++) { if (g_terrainImages[k].data) VirtualFree(g_terrainImages[k].data, 0, MEM_RELEASE); memset(&g_terrainImages[k], 0, sizeof g_terrainImages[k]); }
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
    g_terrainImages[k].key = key; g_terrainImages[k].data = copy; memcpy(g_terrainImages[k].desc, desc, sizeof desc);
    return g_terrainImages[k].desc;
}

static float TerrainTexture(void* texture, uint64_t position)
{
    if (g_terrainThread != 0 && g_terrainThread == GetCurrentThreadId() && g_world.values != NULL)
    {
        texture = TerrainImage(texture);
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

/* ---- Near-player terrain: a 2048 m tile at vanilla's 0.5 m per sample around the drop point --------------------------
 * The level's terrain covers 16 km with one 4096^2 heightmap (4 m a sample): good from afar, coarse underfoot. Around the
 * drop point a second terrain, 2048 m on a side, gets its own 4096^2 height and material maps (vanilla's density),
 * rendered by the level's own terrain merge a second time:
 *
 * 1. At the level's CleanupTerrains (0x65, after its RenderFinalize 0x64 wrote the two named globals and before 0x66
 *    reads them back and tears the merge down) the drop point is chosen from the level's capture (lg+0xC0, float metres,
 *    row 0 at +y) and the water levels (0x0A), and the job queue [ctx+0x50] is waited empty.
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
 *    smoothing, 0x94 flattening lookup (counter record+0xB0 zeroed), 0x3E flatten zones (record+0x80..+0xBF zeroed; the
 *    intermediate captures lg+0..+0xBF it reads are the ones the level's 0x3E read, copied then), 0x3F stamp pads
 *    (record+0x128..+0x167 zeroed), 0x3D zone heights (materials repeating every 2048 m, as on a vanilla map), 0x0D route
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
#define NEAR_SIDE 2048.0f
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
enum { NEAR_IDLE, NEAR_START, NEAR_STEP, NEAR_DRAIN, NEAR_READBACK, NEAR_SPAWN, NEAR_READY, NEAR_IN, NEAR_FAILED };
enum { NEAR_RECORD, NEAR_TILE };

static NearSpawnFn g_nearSpawnOriginal;
static volatile DWORD g_nearSpawnThread; /* the thread spawning the tile (InitSpawnTerrain called by TerrainNearStep) */
static uint64_t* g_nearSlots[2];         /* the render config's slots for the two names (exe 0xA6E30), checked before the spawn */
static NearRunFn g_nearRunOriginal;
static volatile LONG g_nearActive; /* the replay's jobs are queued: the wrappers apply the tile's transform */
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
    uint32_t readId; float* heights; uint32_t hw, hh;
    uint8_t one69[0x19B0], one6B[0x19B0]; uint32_t at69, at6B;
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

/* WorldCamSetPosition: a replayed job's own camera position (the consumer's call at game 0xAB0721) moves to the tile */
static int TerrainNearCamShift(uint64_t camera, uintptr_t at, const float* v, float* out)
{
    if (!g_nearActive || at != g_world.game + 0xAB0725u || (camera != g_near.cam[0] && camera != g_near.cam[1])) return 0;
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
    uintptr_t lg = 0;
    if (SafeRead(context + 0x18, &lg, 8) || lg == 0 || SafeRead(lg, g_near.caps, sizeof g_near.caps)) { g_near.capLevel = -1; return; }
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
    /* the tile's centre: the drop point on a 256 m grid, so the tile is whole nodes of the level terrain's height-bounds
     * quadtree (4096 samples over 16 km: a level-6 node is 64 samples, 256 m; the coarse terrain under the tile is
     * hidden node by node, NearHideCoarse) */
    g_near.centre[0] = 256.0f * floorf(bestX / 256.0f + 0.5f); g_near.centre[1] = 256.0f * floorf(bestY / 256.0f + 0.5f);
    g_near.ready = 1;
    Note("difficulty: near terrain: drop point (%.0f, %.0f), ground %.2f m (water up to %.2f m), %d dry gentle candidates", (double)bestX, (double)bestY,
        (double)NearGlobalAt(bestX, bestY), (double)g_nearWater.high, candidates);
}

/* The level's merge records the tile replays, in the level's order, and the vanilla-density tiles after its 0x3C. */
static int NearPlan(uint64_t records, uint32_t at)
{
    static const uint8_t ops[] = {0x39, 0x3C, 0x40, 0x94, 0x3E, 0x3F, 0x3D, 0x0D, 0x63, 0x42, 0x41, 0x64};
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
            if (cx + core <= x0 || cx >= x0 + NEAR_SIDE || cy + core <= y0 || cy >= y0 + NEAR_SIDE || !TerrainTileWanted(t)) continue;
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
    if (g_near.R != 0)
    {
        NearCameras(g_near.world_size / 2, g_near.world_size / 2, 0.0f, 0.0f);
        NearGuiFn offset = (NearGuiFn)NearApi(0xD0, 0x258), scale = (NearGuiFn)NearApi(0xD0, 0x270);
        if (offset && scale && g_near.gui) { offset(g_near.gui, 0.0f, 0.0f); scale(g_near.gui, 1.0f, 1.0f); }
    }
    NearNameFn unpublish = (NearNameFn)NearApi(0x10, 0x218);
    for (int k = 0; k < 2; k++) if (g_near.tex[k] != 0 && unpublish != NULL) unpublish(kNearNames[k]);
    if (!ok)
    {
        NearReleaseFn release = (NearReleaseFn)NearApi(0x10, 0x198);
        for (int k = 0; k < 2; k++) if (g_near.tex[k] != 0 && release != NULL) { release(g_near.tex[k]); g_near.tex[k] = 0; }
        g_near.state = NEAR_FAILED;
    }
}

/* One replayed record (or tile) step: 1 when it finished, 0 while it runs, -1 when it failed. */
static int NearRunStep(uint64_t records, uint32_t end, uint64_t context, uint64_t g, uint64_t h)
{
    int s = g_near.step;
    uintptr_t gen = 0;
    if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0) return -1;
    volatile uint32_t index = 0;
    volatile int calls = 0;
    LONG fault = 0;
    if (g_near.stepKind[s] == NEAR_TILE)
    {
        if (!g_near.building)
        {
            if (!(TerrainBuild(g_near.stepTile[s], records, end, context, g, h, gen) && TerrainDrawPrepare(records, gen)))
            {
                g_near.tilesFailed++; g_near.calls = 0;
                return 1; /* the level's own cells stay there */
            }
            g_near.building = 1; g_near.calls = 0;
        }
        fault = WorldTileGuarded(g_terrain.record, context, &index, &g_terrain.phase, g, h, 1, &calls);
        if (fault) return -1; /* the merge's state is unknown after a fault */
        if (index == 0 && ++g_near.calls < TERRAIN_DRAW_CALLS) return 0;
        g_near.building = 0; g_near.calls = 0;
        if (index != 0) g_near.tilesDrawn++; else g_near.tilesFailed++;
        return 1;
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
    }
    uint8_t live[0xC0];
    int swapCaps = g_near.stepOp[s] == 0x3E;
    if (swapCaps) { memcpy(live, (void*)(uintptr_t)g_near.lg, sizeof live); memcpy((void*)(uintptr_t)g_near.lg, g_near.caps, sizeof live); }
    int zone = g_near.stepOp[s] == 0x3D && NearZoneRepeat(1);
    fault = WorldTileGuarded(g_near.one, context, &index, &g_near.phase, g, h, 1, &calls);
    if (zone) NearZoneRepeat(0);
    if (swapCaps) memcpy((void*)(uintptr_t)g_near.lg, live, sizeof live);
    if (fault)
    {
        Note("difficulty: near terrain: replayed op 0x%02X faulted (%08lX at game.dll+0x%llX)", g_near.stepOp[s], (unsigned long)fault,
            (unsigned long long)(g_tileFaultAt - g_world.game));
        return -1;
    }
    g_near.calls++;
    if (index != 0) { g_near.calls = 0; return 1; }
    return g_near.calls < 3000 ? 0 : -1;
}

static uint32_t NearQueued(uint64_t context)
{
    uint64_t queue = 0; uint32_t n = 0;
    if (SafeRead(context + 0x50, &queue, 8) || queue == 0 || SafeRead(queue + 0x10, &n, 4)) return 0xFFFFFFFFu;
    return n;
}

/* At the level's CleanupTerrains (0x65): 1 while the level waits for the tile. */
static int TerrainNearReplay(uint64_t records, uint32_t end, uint32_t at, uint64_t context, uint64_t g, uint64_t h)
{
    if (g_near.level != g_world.levels)
    {
        g_near.level = g_world.levels; g_near.state = NEAR_IDLE; g_near.ready = 0; g_near.handle = 0;
        /* the previous level's tile textures: its world is gone */
        NearReleaseFn release = (NearReleaseFn)NearApi(0x10, 0x198);
        for (int k = 0; k < 2; k++) if (g_near.tex[k] != 0 && release != NULL) release(g_near.tex[k]);
        g_near.tex[0] = g_near.tex[1] = 0;
        if (!NearOn()) { Note("difficulty: near terrain: off (HELLFORGE_XL_NEAR=0)"); return 0; }
        uintptr_t gen = 0, lg = 0, settings = 0, data = 0;
        uint32_t dims[2] = {0};
        if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0 || SafeRead(context + 0x18, &lg, 8) || lg == 0 || SafeRead(context + 8, &settings, 8) ||
            settings == 0 || SafeRead(lg + 0xC0, dims, 8) || SafeRead(lg + 0xD0, &data, 8) || data == 0 || dims[0] != 4096 || dims[1] != 4096)
        { Note("difficulty: near terrain: the level's capture is not the reviewed one (%u x %u); no tile", dims[0], dims[1]); return 0; }
        size_t bytes = (size_t)dims[0] * dims[1] * 4;
        if (g_near.global == NULL) g_near.global = (float*)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (g_near.global == NULL || SafeRead(data, g_near.global, bytes)) { Note("difficulty: near terrain: the level's heightmap did not copy; no tile"); return 0; }
        g_near.columns = dims[0]; g_near.rows = dims[1];
        TerrainNearDrop();
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
            g_near.tex[k] = create(w, hgt, mips, format, 0x48460000u + (uint32_t)k, flags);
            if (g_near.tex[k] == 0) { Note("difficulty: near terrain: texture %d was not created; no tile", k); NearReplayEnd(0); return 0; }
            Note("difficulty: near terrain: tile texture %d: %u x %u, %u mips, format %u, flags 0x%X (as the config's 0x%08X)", k, w, hgt, mips, format, flags, kNearNames[k]);
        }
        for (int k = 0; k < 2; k++) publish(kNearNames[k], g_near.tex[k]);
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
        int r = NearRunStep(records, end, context, g, h);
        if (r < 0)
        {
            Note("difficulty: near terrain: step %d (op 0x%02X) failed; no tile", g_near.step, g_near.stepOp[g_near.step]);
            NearReplayEnd(0);
            return 0;
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
        if (data == 0 || w != 4096 || hgt != 4096 || NearTexelBytes(format) != 4)
        { Note("difficulty: near terrain: the tile's readback is %u x %u format %u; no tile", w, hgt, format); NearReplayEnd(0); return 0; }
        size_t bytes = (size_t)w * hgt * 4;
        if (g_near.heights == NULL) g_near.heights = (float*)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (g_near.heights == NULL || SafeRead(data, g_near.heights, bytes)) { NearReplayEnd(0); return 0; }
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
        NearReplayEnd(1);
        Note("difficulty: near terrain: the tile is rendered in %.1f s (%u frames; %d vanilla-density tiles drawn, %d failed): heights %.1f..%.1f m, mean %.1f, "
            "%u of %u samples zero; mean |tile - level| %.2f m", (double)(GetTickCount64() - g_near.started) / 1000.0, g_near.frames, g_near.tilesDrawn,
            g_near.tilesFailed, (double)lo, (double)hi, n ? sum / n : 0.0, zeros, n, n ? diff / n : 0.0);
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
        if (g_nearSlots[k] == NULL || SafeRead((uintptr_t)g_nearSlots[k], &now[k], 8) || now[k] != g_near.config[k] ||
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
    size[0] = NEAR_SIDE - 0.5f; size[1] = NEAR_SIDE - 0.5f;
    uint64_t keep[2] = {*g_nearSlots[0], *g_nearSlots[1]};
    for (int k = 0; k < 2; k++) *g_nearSlots[k] = g_near.tex[k];
    uint64_t r = g_nearSpawnOriginal(world, pose, a, type, name, resource, hash, size, range, flag);
    for (int k = 0; k < 2; k++) *g_nearSlots[k] = keep[k];
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
static void NearHideCoarse(uint64_t world)
{
    uintptr_t gen = 0, h = 0, dimsAt = 0, cow = 0, nodes = 0;
    uint32_t W = 0, H = 0, count = 0;
    if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0 || SafeRead(gen + 0x8758A0u, &h, 8) || h == 0 || SafeRead(h + 0x58, &dimsAt, 8) ||
        dimsAt == 0 || SafeRead(dimsAt + 0x14, &W, 4) || SafeRead(dimsAt + 0x18, &H, 4) || W != 4096 || H != 4096)
    { Note("difficulty: near terrain: the level terrain is not the reviewed one (%u x %u); the coarse ground stays under the tile", W, H); return; }
    const float step = 16384.0f / (float)W;
    int x0 = (int)((g_near.centre[0] - NEAR_SIDE / 2 + 8192.0f) / step + 0.5f), n = (int)(NEAR_SIDE / step + 0.5f);
    int y0 = (int)((8192.0f - (g_near.centre[1] + NEAR_SIDE / 2)) / step + 0.5f);
    if (x0 < 1 || y0 < 1 || x0 + n >= (int)W || y0 + n >= (int)H) { Note("difficulty: near terrain: the tile is not inside the level terrain"); return; }
    typedef void (*EditFn)(uint64_t world, uint64_t h, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const uint16_t* heights, const uint32_t* materials,
        uint32_t w, uint32_t rows);
    EditFn edit = (EditFn)NearApi(0x08, 0x1D0);
    if (edit == NULL) return;
    uint32_t w = (uint32_t)(n - 1);
    uint16_t* low = (uint16_t*)VirtualAlloc(NULL, (size_t)w * w * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); /* zeros: -500 m */
    if (low == NULL) return;
    edit(world, h, x0 + 1, y0 + 1, x0 + n, y0 + n, low, NULL, w, w);
    VirtualFree(low, 0, MEM_RELEASE);
    if (SafeRead(h + 0x40, &cow, 8) || cow == 0 || SafeRead(cow, &count, 4) || SafeRead(cow + 8, &nodes, 8) || nodes == 0 || count == 0 || count > 1u << 20)
    { Note("difficulty: near terrain: the coarse terrain's bounds were not copied; it is still drawn under the tile"); return; }
    /* the tile in quadtree space: columns [x0, x0 + n), B [H - (y0 + n), H - y0) */
    int c0 = x0, c1 = x0 + n, b0 = (int)H - (y0 + n), b1 = (int)H - y0;
    struct { uint32_t node; int c0, c1, b0, b1, inside; } stack[64];
    int top = 0, hidden = 0;
    stack[top].node = 0; stack[top].c0 = 0; stack[top].c1 = (int)W; stack[top].b0 = 0; stack[top].b1 = (int)H; stack[top].inside = 0; top++;
    const float nan = __builtin_nanf("");
    while (top > 0)
    {
        top--;
        uint32_t k = stack[top].node;
        int nc0 = stack[top].c0, nc1 = stack[top].c1, nb0 = stack[top].b0, nb1 = stack[top].b1, inside = stack[top].inside;
        if (k >= count) continue;
        uint8_t* node = (uint8_t*)(uintptr_t)(nodes + (uint64_t)k * 24);
        if (!inside && (nc1 <= c0 || nc0 >= c1 || nb1 <= b0 || nb0 >= b1)) continue;
        if (!inside && nc0 >= c0 && nc1 <= c1 && nb0 >= b0 && nb1 <= b1) inside = 1;
        if (inside) { memcpy(node, &nan, 4); memcpy(node + 4, &nan, 4); hidden++; }
        uint32_t child[4];
        memcpy(child, node + 8, 16);
        if (child[0] == 0xFFFFFFFFu) continue;
        int cm = (nc0 + nc1) / 2, bm = (nb0 + nb1) / 2;
        const int box[4][4] = {{nc0, cm, nb0, bm}, {cm, nc1, nb0, bm}, {cm, nc1, bm, nb1}, {nc0, cm, bm, nb1}};
        for (int q = 0; q < 4 && top < 64; q++)
        {
            stack[top].node = child[q]; stack[top].c0 = box[q][0]; stack[top].c1 = box[q][1]; stack[top].b0 = box[q][2]; stack[top].b1 = box[q][3];
            stack[top].inside = inside; top++;
        }
    }
    Note("difficulty: near terrain: the coarse terrain under the tile is hidden (%d quadtree nodes) and its collision lowered (samples %d..%d, rows %d..%d)",
        hidden, x0 + 1, x0 + n - 1, y0 + 1, y0 + n - 1);
}

static int TerrainNearStep(uint64_t records, uint32_t end, uint32_t at, uint32_t op, uint64_t context, uint64_t g, uint64_t h)
{
    if (!TerrainOn() || g_world.current != 8.0f) return 0;
    if (op == 0x3E) { NearKeepCaptures(context); return 0; }
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
        return 0;
    }
    if (g_near.at6B == 0 || at <= g_near.at6B) return 0;
    if (g_near.state == NEAR_SPAWN)
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
        g_near.state = NEAR_READY; g_near.polls = 0;
        return 1;
    }
    if (g_near.state == NEAR_READY)
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
        NearHideCoarse(world);
        g_near.state = NEAR_IN;
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
