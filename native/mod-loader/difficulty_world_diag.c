/* Terrain diagnostics (research): what every level's generation does to the terrain, measured, so a map that comes out
 * smooth, flat, stretched or bare can be traced to the pass that made it so. Off unless HELLFORGE_TERRAIN_DIAG is set:
 *
 *   1  measurements and a report per level (any level: a vanilla map gives the baseline an extra large one is held to)
 *   2  also reads the merge's named render targets back after the passes that draw into them, and writes every image it
 *      measured (raw, with a descriptor) for scripts/terrain-diag/terrain_diag.py to render and compare
 *
 * HELLFORGE_TERRAIN_DIAG_READBACK=off keeps level 2's image files but skips the readbacks (each read back image stays
 * allocated by the engine: up to ~350 MB per level).
 *
 * What it records, per level (g_world.levels), into terrain-diag-<level>.json in the session's logs and a summary in
 * modloader.log:
 *
 *   settings  the level's world size (lg+0x1E0) and radii (settings = lg+0x2D20: +0x50..+0x6C), the merge's tiles
 *             (G+0x8758A8 +0x120 / +0x124), its samples per side (op 0x38's record +0xB4), the map type, the switches
 *   plan      before the first merge (op 0x38): the main Voronoi's vertices (G+0x9F310, 0x48 each; heights G+0x1F8A30;
 *             count G+0x11A7090): their spacing (nearest neighbour, m) and height spread; subregion cells (G+0x92F328,
 *             0x20 each; count G+0x11A71C4) by height group and type; locations (G+0x11A7108); route vertices, cells,
 *             edges (G+0x11A71B4 / +0x11A71C0 / +0x11A71CC)
 *   ops       every pass record: op, name, steps (dispatcher calls), wall time, frames from first to last step, and the
 *             locations it added (stamps are placed as locations)
 *   images    each image measured, from:
 *               - the merge's 8 intermediate captures (G+0x119FF68, 0x18 each) after each op 0x5C
 *               - readbacks of the named targets (level 2, between SetupTerrainMerge and CleanupTerrainMerge, calls as
 *                 op 0x66 makes them at 0xF544F4..0xF544B5): heights 0x2A9E3232 after the passes that draw heights,
 *                 materials 0x95E7E3AC after 0x41 / 0x64, the zone lookup 0x1524FE5A after op 0x4C draws it
 *               - the scatter images (lg = G+0x8758A8: +0xD8 heights u16, +0xF0 material mix, +0x108 ground ids) after
 *                 op 0x66 (heights, materials) and op 0xBB (ground ids, made by op 0x47: 0xBB fills the id table)
 *               - the layer heightfield the loader keeps (difficulty_world.c, the terrain the layers snap to) at op 0x6D
 *             Heights: in-disk spread and percentiles; slopes (histogram, mean, flat and steep shares); curvature; detail
 *             at 8 / 32 / 128 / 512 / 2048 m (sd of the height less its local mean); local relief (max - min) in 32 /
 *             128 / 512 m blocks; correlation length along x and y (stretching shows as a longer length, uneven
 *             stretching as x != y). Material mix: painted share, weights, coverage per material slot. Ground ids:
 *             density, coverage per id (through the scatter id table). Anything else: value spread and distinct values.
 *   flags     health checks on the final terrain: resolution, flatness, missing detail, stretched lattice, anisotropy,
 *             an unpainted material map, sparse scatter, a scatter cutoff inside the disk, stretched zone materials
 *
 * A readback copies the target when the GPU reaches the request, which can be several passes later: a readback "after"
 * a pass shows the target at some point after it, not exactly then (the final images are the reliable ones).
 *
 * The images are measured inside the level's disk (lg+0x2D7C x half the world, around the centre), the playable land.
 * The readbacks are asynchronous: requested as a pass's last step returns, collected on later dispatcher calls or game
 * frames (TerrainDiagTick, packages.c), the report written once generation has ended and nothing is pending (or 20 s
 * after). Everything runs on the thread that calls it; the state is behind one lock. */

static const char* const kTdOpNames[0xBC] = {
    "ApplyZoneDeltaSettings", "SetupMissionDistance", "BuildRouteCellAssociatedRoutes", "GenerateRouteCellVoronoi",
    "GenerateSubregionHeights", "SmoothenRouteConnections", "GenerateObjectiveForcedLocations", "GenerateRoutes",
    "AdjustRoutesForZones", "PlaceStampsOnRoutes", "GenerateWaterStart", "GenerateWaterFinalize", "FixWaterHeight",
    "RenderRouteSegments", "GenerateRouteSegments", "AddRoutesToCollision", "AddRoadsToCollision", "SelectSubRegionCellType",
    "SortRouteCellRoutes", "MergeSubRegionCells", "LevelGenerationNop", "GenerateMissionStampList", "GenerateObjectiveLocations",
    "AddImportantLocations", "GenerateLocationClusters", "ForceSeed", "SpawnSettingsEntities", "BuildPalette", NULL,
    "WaitForPackageGroup", NULL, "QueueStampPackages", NULL, "QueueRandomCacheLootRewardsPackages", NULL,
    "QueueMarkerPackages", NULL, "GenerateVoronoi", "GenerateFbr", "PaintZoneByCornerTag", "PaintZoneByLocation",
    "CellularAutomata", "CellularAutomataCorridor", "PlaceStampWithWeightRule", "CalculateCornerDownhill",
    "CalculateCornerSlopes", "CalculateAreaSlopes", "SpawnStampTerrain", "ExtendStamps", NULL, "SpawnVistaLevel",
    "GenerateSky", "GenerateSkyTestLevel", "GenerateVista", "GatherIndoorVolumes", "KickTerrainMerge", "SetupTerrainMerge",
    "MergeClear", NULL, "DestroyVoronoiGeometries", "RenderHeightmapVoronoi", "RenderZoneHeightmap", "ForceFlattenZones",
    "RenderStampsHeightmap", "SmoothHeightmap", "RenderVoronoiMaterials", "GenerateHeightmapProperties", "SetupFogVolumes",
    "RenderFogVolumes", "CleanupFogVolumes", "SetupTerrainLightingMapsRenderData", "RenderTerrainLightingMaps",
    "CleanupTerrainLightingMaps", "SetupFarScatterRenderData", "RenderFarScatter", "CleanupFarScatter", "RenderZoneLookup",
    "SpawnEarthquakes", "GenerateOutsideMapDistance", "SetupIngameMapRenderData", "CreateIngameMap", "RenderPlayableArea",
    "CleanupIngameMap", "AllocateMinimapResources", "ReleaseMinimapResources", "CollectMinimapProxies",
    "CaptureMinimapTexture", "CollectMinimapScatter", "SpawnProxiesForCapture", "DespawnProxies", "CreateMinimapTexture",
    "CaptureIntermediateHeightmapStart", "CaptureIntermediateHeightmapFinalize", "CaptureHeightmapStart",
    "CaptureHeightmapFinalize", "GenerateResourceHash", "StartLoadMinimapResources", "UnloadUnusedMinimapPackages",
    "FilterMinimap", "MergeStampHeights", "RenderFinalize", "CleanupTerrains", "CleanupTerrainMerge",
    "CleanupTerrainMergeDelayed", "NavmeshGridInitialize", "InitSpawnTerrain", "SpawnTerrainWork", "SpawnTerrainFinalize",
    "SpawnLevelLayers", "CullUnitsOutsidePlayableArea", "EnableConcurrentTerrainNavmeshSimplification", NULL,
    "SetupTerrainNavmesh", "SetupNavmeshGeometry", "SetLevelToLoad", "LoadLevel", "GenerateSubregionCellsHeightGroupBasic",
    "InitialiseSubregionCells", "GenerateSubregionSpecial", "GenerateSubregionCellsHeightGroupFromLow",
    "SelectSubRegionCellsFromHeightGroup", "PaintVoronoiCornerByRouteCell", "PaintRouteCellsAsSubRegion",
    "LinkLocationsToRouteCells", "AttachLocationsToRouteCells", "FixUnreachableAreas", "PaintSubRegionZones",
    "TestLevelZoneSetup", "GenerateLocations", "GenerateDiscoveryLocations", "GenerateOutpostLocations",
    "CalculateValidSmallLocationsPositions", "GenerateSmallLocations", "GenerateEmptyBorderLocations",
    "GeneratePriorityRoutes", "GenerateConvoyRoutes", "GenerateCrossingLocations", "GenerateRoadAlongRoutes",
    "FlattenVoronoiFromRoads", "SpreadLocations", "SpreadLocations_Cities", "FinalizeLocationPlacements",
    "AddSatelliteLocations", NULL, "SetupRegion", "GenerateVoronoiHeightDifferenceMap", "GenerateHeightFromLocations",
    "GenerateBottomFlatLookup", "GenerateFlatteningLookup", "InitializeScatter", "FinalizeScatterInitialization",
    "CalculateScatterCache", "FreezeAllPlacedLocations", "OptimizeLocationRotation", "ForceSpawnDebugLocation",
    "SetupBackgroundWind", "SpreadEdgeOfMapLocations", "GenerateDangerLevels", "CalculateMissionBoundaries",
    "SelectOceanDirection", "GenerateExplorationLocations", "GatherExplorationLevels", "SpawnExplorationLevels",
    "RevoronoiNatureRoutes", "FinalizeVoronoi", "QueueZonePackages", NULL, "GenerateCityRoads", "FlattenCityRoads",
    "FlattenCityRegions", "FlattenCityStamps", "PlaceCityRoadStamps", "SetStampFlattenModeForCityRegions",
    "ModifyCityRoutesThroughMissionStamps", "GenerateRoutesClassLookup", "GenerateCityWalls", "ProcessAsyncOperations",
    "SetupHellpodAvoidance", "GenerateClearingMask", "GenerateClearingPoints", "GenerateHiveLordGraph",
    "GenerateHiveLordGraphPost", "SubRegionExpansion", "LevelRoutes", "SpawnMeshesOnEmbankments", "ForceStructuredZoneLayout",
    "SpawnImpTerrainProjector", "InitializeGlobalScatterIndirection"};

static const char* TdOpName(uint32_t op) { return op < 0xBC && kTdOpNames[op] ? kTdOpNames[op] : "?"; }

#define TD_RT_HEIGHTS 0x2A9E3232u   /* the terrain's height target (op 0x66 reads it back into lg+0xD8) */
#define TD_RT_MATERIALS 0x95E7E3ACu /* the material mix target (op 0x66 -> lg+0xF0; the GPU scatter samples it, 0xF45106) */
#define TD_RT_ZONES 0x1524FE5Au     /* the zone lookup target RenderZoneLookup (op 0x4C) creates and draws */
#define TD_IMAGE_BLOCK 0x8758A8u    /* G+: the scatter images' owner (op 0xBB's [record+0x20]), +0x120 tile samples, +0x124 tiles */
#define TD_MAX_RECORDS 512
#define TD_MAX_PENDING 24

typedef enum { TD_HEIGHTS_U16, TD_HEIGHTS_F32, TD_MATERIALS, TD_GROUND, TD_ZONES, TD_RAW } TdKind;

typedef struct TdOp { uint32_t op, steps, locBefore, locAfter, done; double ms; ULONGLONG first, last; } TdOp;
typedef struct TdPending { uint32_t id, hash, record, op; TdKind kind; ULONGLONG since; char label[48]; } TdPending;
typedef struct TdKey { int set; double metresPerSample, sd, flat, detail32, detail128, corrX, corrY, slopeMean; } TdKey;

static struct
{
    SRWLOCK lock;
    int mode, readback;                  /* HELLFORGE_TERRAIN_DIAG 0..2, readbacks on */
    LONG level;                          /* g_world.levels the state belongs to */
    int planned, reported, ended, merging; /* merging: SetupTerrainMerge (0x38) done, CleanupTerrainMerge (0x66) not yet */
    ULONGLONG endedAt;
    uint32_t end, records, frames;
    TdOp ops[TD_MAX_RECORDS];
    uint32_t stepRecord, stepLocations;
    LARGE_INTEGER stepStart;
    TdPending pending[TD_MAX_PENDING];
    uint32_t pendingCount, readbacks, images, captureSets;
    LONG sessionReadbacks;
    char settings[4096], plan[8192], imagesJson[96 * 1024], flags[4096];
    int settingsLen, planLen, imagesLen, flagsLen;
    TdKey finalHeights, imageHeights;    /* the layer heightfield's, the scatter heights image's */
    double planSpacing, materialPainted, groundDensity, scatterRadius, disk, world;
    int materialSeen, groundSeen;
    uint64_t lastData[64];               /* images already measured this level (a sample hash: no repeats of an unchanged capture) */
    uint32_t lastDataCount;
} g_td = {SRWLOCK_INIT, -1};

static ULONGLONG g_tdFrameTick;

static int TdAppend(char* buffer, int* length, int size, const char* format, ...)
{
    if (*length >= size - 1) return 0;
    va_list arguments;
    va_start(arguments, format);
    int n = vsnprintf(buffer + *length, (size_t)(size - *length), format, arguments);
    va_end(arguments);
    if (n < 0) return 0;
    *length += n < size - *length ? n : size - *length - 1;
    return n;
}

/* A JSON number, or null for a value that is not one. */
/* A correlation length: beyond the search (negative, see TdCorrelation) as the string ">N". */
static const char* TdLength(char* out, size_t size, double v)
{
    if (v == v && v < 0) { snprintf(out, size, "\">%.0f\"", -v); return out; }
    if (!(v == v)) { snprintf(out, size, "null"); return out; }
    snprintf(out, size, "%.6g", v);
    return out;
}

static const char* TdNum(char* out, size_t size, double v)
{
    if (!(v == v) || v > 1e300 || v < -1e300) snprintf(out, size, "null");
    else snprintf(out, size, "%.6g", v);
    return out;
}

static uintptr_t TdGen(void)
{
    uintptr_t gen = 0;
    if (g_worldLevel == 0 || SafeRead(g_worldLevel + 0x2C8, &gen, 8)) return 0;
    return gen;
}

/* ---- the level's frame and settings ----------------------------------------------------------------------------- */

static void TdLevelReset(void)
{
    LONG level = g_world.levels;
    memset(g_td.ops, 0, sizeof g_td.ops);
    g_td.level = level;
    g_td.planned = g_td.reported = g_td.ended = g_td.merging = 0;
    g_td.endedAt = 0;
    g_td.end = g_td.records = g_td.frames = 0;
    g_td.pendingCount = g_td.readbacks = g_td.images = g_td.captureSets = 0;
    g_td.settingsLen = g_td.planLen = g_td.imagesLen = g_td.flagsLen = 0;
    g_td.settings[0] = g_td.plan[0] = g_td.imagesJson[0] = g_td.flags[0] = 0;
    memset(&g_td.finalHeights, 0, sizeof g_td.finalHeights);
    memset(&g_td.imageHeights, 0, sizeof g_td.imageHeights);
    g_td.planSpacing = g_td.materialPainted = g_td.groundDensity = g_td.scatterRadius = 0;
    g_td.materialSeen = g_td.groundSeen = 0;
    g_td.lastDataCount = 0;
    g_td.world = 2048.0 * g_world.current;
    g_td.disk = 0.0;
    if (g_worldLevel != 0)
    {
        float world = 0, disk = 0;
        if (!SafeRead(g_worldLevel + 0x1E0, &world, 4) && world >= 256.0f && world <= 65536.0f) g_td.world = world;
        if (!SafeRead(g_worldLevel + 0x2D7C, &disk, 4) && disk > 0.0f && disk <= 1.0f) g_td.disk = disk * g_td.world * 0.5;
    }
}

static void TdSettings(uint64_t record)
{
    uintptr_t lg = g_worldLevel, gen = TdGen();
    if (lg == 0) return;
    float world = 0, radii[8] = {0}, tileSamples = 0, samples = 0;
    uint32_t worldInt = 0, tiles = 0, mapType = 0;
    uint8_t difficulty = 0;
    SafeRead(lg + 0x1E0, &world, 4); SafeRead(lg + 0x2D4C, &worldInt, 4); SafeRead(lg + 0x2D70, radii, sizeof radii);
    SafeRead(lg + 0xF9, &difficulty, 1); SafeRead(lg + 0x2DE0, &mapType, 4);
    if (gen != 0) { SafeRead(gen + TD_IMAGE_BLOCK + 0x120, &tileSamples, 4); SafeRead(gen + TD_IMAGE_BLOCK + 0x124, &tiles, 4); } /* +0x120 is a float */
    if (record != 0) SafeRead(record + 0xB4, &samples, 4);
    if (radii[3] > 0.0f && radii[3] <= 1.0f) g_td.disk = radii[3] * (world > 0 ? world : g_td.world) * 0.5;
    if (world >= 256.0f) g_td.world = world;
    char zone[8] = "", radius[8] = "";
    GetEnvironmentVariableA("HELLFORGE_XL_ZONE_REPEAT", zone, sizeof zone);
    GetEnvironmentVariableA("HELLFORGE_XL_RADIUS_OLD", radius, sizeof radius);
    double half = g_td.world * 0.5;
    char a[32], b[32], c[32], d[32], e[32], f[32], g[32], h[32];
    g_td.settingsLen = 0;
    TdAppend(g_td.settings, &g_td.settingsLen, sizeof g_td.settings,
        "{\"world_m\": %s, \"world_int\": %u, \"world_scale\": %g, \"difficulty\": %u, \"map_type_raw\": \"0x%08X\", "
        "\"radius_50\": %s, \"radius_54\": %s, \"fade_58\": %s, \"disk_5c\": %s, \"radius_60\": %s, \"rect_low_m\": %s, "
        "\"rect_high_m\": %s, \"disk_m\": %g, \"fade_m\": %g, \"vista_fade_m\": %g, \"merge_tiles\": %u, \"tile_samples\": %g, "
        "\"merge_samples_per_side\": %g, \"metres_per_sample\": %g, \"zone_repeat\": %s, \"radius_old\": %s}",
        TdNum(a, 32, world), worldInt, (double)g_world.current, (unsigned)difficulty, mapType, TdNum(b, 32, radii[0]), TdNum(c, 32, radii[1]),
        TdNum(d, 32, radii[2]), TdNum(e, 32, radii[3]), TdNum(f, 32, radii[4]), TdNum(g, 32, radii[5]), TdNum(h, 32, radii[6]),
        radii[3] * half, radii[2] * half, (radii[3] - radii[2]) * half, tiles, (double)tileSamples, (double)samples,
        tiles && tileSamples > 0 ? g_td.world / ((double)tiles * tileSamples) : 0.0, zone[0] == '1' ? "true" : "false",
        radius[0] == '1' ? "true" : "false");
}

/* ---- the plan, before the first merge --------------------------------------------------------------------------- */

static int TdFloatCompare(const void* a, const void* b) { float x = *(const float*)a, y = *(const float*)b; return x < y ? -1 : x > y; }

static void TdPlan(void)
{
    uintptr_t gen = TdGen();
    if (gen == 0) return;
    uint32_t vertices = 0, subs = 0, locations = 0, route[3] = {0};
    SafeRead(gen + 0x11A7090u, &vertices, 4); SafeRead(gen + 0x11A71C4u, &subs, 4); SafeRead(gen + 0x11A7108u, &locations, 4);
    SafeRead(gen + 0x11A71B4u, &route[0], 4); SafeRead(gen + 0x11A71C0u, &route[1], 4); SafeRead(gen + 0x11A71CCu, &route[2], 4);
    if (vertices > 65536) vertices = 0;
    static float vxy[65536][2], vh[65536], nn[65536];
    uint32_t n = 0;
    for (uint32_t v = 0; v < vertices; v++)
    {
        if (SafeRead(gen + 0x9F310u + 0x48u * (uintptr_t)v, vxy[n], 8) || SafeRead(gen + 0x1F8A30u + 4u * (uintptr_t)v, &vh[n], 4)) break;
        if (vxy[n][0] == vxy[n][0] && vh[n] == vh[n] && vh[n] > -1e6f && vh[n] < 1e6f) n++;
    }
    /* nearest-neighbour spacing on a 64 m hash grid (the lattice is 16 m at vanilla density) */
    double world = g_td.world;
    int grid = (int)(world / 64.0); if (grid < 8) grid = 8; if (grid > 512) grid = 512;
    double cell = world / grid;
    static uint32_t heads[512 * 512], next[65536];
    for (int i = 0; i < grid * grid; i++) heads[i] = 0xFFFFFFFFu;
    for (uint32_t v = 0; v < n; v++)
    {
        int gx = (int)(vxy[v][0] / cell), gy = (int)(vxy[v][1] / cell);
        gx = gx < 0 ? 0 : gx >= grid ? grid - 1 : gx;
        gy = gy < 0 ? 0 : gy >= grid ? grid - 1 : gy;
        next[v] = heads[gy * grid + gx]; heads[gy * grid + gx] = v;
    }
    uint32_t m = 0;
    double sum = 0, sum2 = 0, lo = 1e30, hi = -1e30, zeros = 0;
    for (uint32_t v = 0; v < n; v++)
    {
        int gx = (int)(vxy[v][0] / cell), gy = (int)(vxy[v][1] / cell);
        double best = 1e30;
        for (int r = 1; r <= 4 && best > (r - 1) * cell * (r - 1) * cell; r++)
            for (int y = gy - r; y <= gy + r; y++)
                for (int x = gx - r; x <= gx + r; x++)
                {
                    if (x < 0 || y < 0 || x >= grid || y >= grid) continue;
                    for (uint32_t u = heads[y * grid + x]; u != 0xFFFFFFFFu; u = next[u])
                    {
                        if (u == v) continue;
                        double dx = vxy[u][0] - vxy[v][0], dy = vxy[u][1] - vxy[v][1], d = dx * dx + dy * dy;
                        if (d < best && d > 1e-6) best = d;
                    }
                }
        if (best < 1e29) nn[m++] = (float)sqrt(best);
        sum += vh[v]; sum2 += (double)vh[v] * vh[v]; if (vh[v] < lo) lo = vh[v]; if (vh[v] > hi) hi = vh[v]; if (vh[v] == 0.0f) zeros++;
    }
    double nnMean = 0;
    for (uint32_t k = 0; k < m; k++) nnMean += nn[k];
    nnMean = m ? nnMean / m : 0;
    if (m) qsort(nn, m, sizeof nn[0], TdFloatCompare);
    double nnMedian = m ? nn[m / 2] : 0;
    g_td.planSpacing = nnMedian;
    double mean = n ? sum / n : 0, sd = n ? sqrt(fmax(0.0, sum2 / n - mean * mean)) : 0;
    /* subregion cells by height group (-1..6) and by type */
    uint32_t groups[8] = {0}, types[256] = {0};
    for (uint32_t i = 0; i < subs && i < 8192; i++)
    {
        uint8_t entry[0x20];
        if (SafeRead(gen + 0x92F328u + 0x20u * (uintptr_t)i, entry, sizeof entry)) break;
        int32_t group, type; memcpy(&group, entry + 4, 4); memcpy(&type, entry + 0xC, 4);
        if (group >= -1 && group <= 6) groups[group + 1]++;
        if (type >= 0 && type < 256) types[type]++;
    }
    char a[32], b[32], c[32], d[32];
    g_td.planLen = 0;
    TdAppend(g_td.plan, &g_td.planLen, sizeof g_td.plan,
        "{\"vertices\": %u, \"vertices_per_km2\": %g, \"spacing_mean_m\": %s, \"spacing_median_m\": %s, "
        "\"height_min\": %s, \"height_max\": %s, \"height_mean\": %g, \"height_sd\": %g, \"height_zero_share\": %g, "
        "\"subregion_cells\": %u, \"locations\": %u, \"route_vertices\": %u, \"route_cells\": %u, \"route_edges\": %u, \"groups\": {",
        n, world > 0 ? n / (world * world / 1e6) : 0.0, TdNum(a, 32, nnMean), TdNum(b, 32, nnMedian), TdNum(c, 32, n ? lo : NAN),
        TdNum(d, 32, n ? hi : NAN), mean, sd, n ? zeros / n : 0.0, subs, locations, route[0], route[1], route[2]);
    int first = 1;
    for (int k = 0; k < 8; k++) if (groups[k]) { TdAppend(g_td.plan, &g_td.planLen, sizeof g_td.plan, "%s\"%d\": %u", first ? "" : ", ", k - 1, groups[k]); first = 0; }
    TdAppend(g_td.plan, &g_td.planLen, sizeof g_td.plan, "}, \"types\": {");
    first = 1;
    for (int k = 0; k < 256; k++) if (types[k]) { TdAppend(g_td.plan, &g_td.planLen, sizeof g_td.plan, "%s\"0x%X\": %u", first ? "" : ", ", k, types[k]); first = 0; }
    TdAppend(g_td.plan, &g_td.planLen, sizeof g_td.plan, "}}");
    Note("terrain diag (level %ld, %.0fx): plan: %u Voronoi vertices (%.1f per km2), spacing median %.1f m (mean %.1f), heights %.4f..%.4f sd %.4f (%.0f%% at 0), %u subregion cells, %u locations, routes %u/%u/%u",
        (long)g_td.level, (double)g_world.current, n, world > 0 ? n / (world * world / 1e6) : 0.0, nnMedian, nnMean, n ? lo : 0.0, n ? hi : 0.0, sd,
        n ? 100.0 * zeros / n : 0.0, subs, locations, route[0], route[1], route[2]);
}

/* ---- image measurements ----------------------------------------------------------------------------------------- */

static float* g_tdField;           /* the full-resolution heights being measured (m) */
static size_t g_tdFieldCap;
static float* g_tdCoarse;          /* <= 1024 x 1024 block means */
static double* g_tdSum;            /* the coarse grid's integral image */
static uint8_t* g_tdMask;          /* coarse cells inside the disk */

static int TdReserve(size_t samples)
{
    if (samples <= g_tdFieldCap) return 1;
    if (g_tdField) VirtualFree(g_tdField, 0, MEM_RELEASE);
    g_tdField = (float*)VirtualAlloc(NULL, samples * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_tdFieldCap = g_tdField ? samples : 0;
    if (!g_tdCoarse) g_tdCoarse = (float*)VirtualAlloc(NULL, 1024u * 1024u * 4u, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_tdSum) g_tdSum = (double*)VirtualAlloc(NULL, 1025u * 1025u * 8u, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_tdMask) g_tdMask = (uint8_t*)VirtualAlloc(NULL, 1024u * 1024u, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return g_tdField && g_tdCoarse && g_tdSum && g_tdMask;
}

/* The box mean of the coarse grid around (x, y), half-width k cells, clipped at the edges. */
static double TdBox(int d, int x, int y, int k)
{
    int x0 = x - k < 0 ? 0 : x - k, y0 = y - k < 0 ? 0 : y - k, x1 = x + k + 1 > d ? d : x + k + 1, y1 = y + k + 1 > d ? d : y + k + 1;
    double s = g_tdSum[(size_t)y1 * (d + 1) + x1] - g_tdSum[(size_t)y0 * (d + 1) + x1] - g_tdSum[(size_t)y1 * (d + 1) + x0] + g_tdSum[(size_t)y0 * (d + 1) + x0];
    return s / ((double)(x1 - x0) * (y1 - y0));
}

/* The correlation length (m) along x (dx 1) or y: the lag at which the autocorrelation of the detrended coarse grid falls
 * below 1/e, interpolated; minus a quarter of the grid (in m) if it never does within that (features longer than the
 * search: the report shows it as a string ">N"); NaN if too few samples. */
static double TdCorrelation(int d, double cellM, const float* e, double var, int alongX)
{
    if (var <= 0) return NAN;
    double prev = 1.0, target = 1.0 / 2.718281828;
    int step = d > 512 ? 2 : 1;
    for (int lag = 1; lag <= d / 4; lag++)
    {
        double s = 0; size_t count = 0;
        for (int y = 0; y < d; y += step)
            for (int x = 0; x < d; x += step)
            {
                int x2 = alongX ? x + lag : x, y2 = alongX ? y : y + lag;
                if (x2 >= d || y2 >= d) continue;
                size_t i = (size_t)y * d + x, j = (size_t)y2 * d + x2;
                if (!g_tdMask[i] || !g_tdMask[j]) continue;
                s += (double)e[i] * e[j]; count++;
            }
        if (count < 64) return NAN;
        double c = s / count / var;
        if (c < target) return (lag - 1 + (prev - target) / (prev - c)) * cellM;
        prev = c;
    }
    return -(d / 4) * cellM;
}

/* Height metrics of g_tdField (w x h, metresPerSample, the disk of radius diskM around the image's centre). */
static void TdHeights(uint32_t w, uint32_t h, double metres, double diskM, char* json, int* len, int size, TdKey* key)
{
    const float* f = g_tdField;
    double cx = w * 0.5, cy = h * 0.5, r = diskM > 0 ? diskM / metres : (w < h ? w : h) * 0.5, r2 = r * r;
    /* spread */
    double sum = 0, sum2 = 0, lo = 1e30, hi = -1e30; size_t n = 0;
    for (uint32_t y = 0; y < h; y++)
    {
        double dy = y + 0.5 - cy;
        for (uint32_t x = 0; x < w; x++)
        {
            double dx = x + 0.5 - cx;
            if (dx * dx + dy * dy > r2) continue;
            double v = f[(size_t)y * w + x];
            if (!(v == v)) continue;
            sum += v; sum2 += v * v; n++; if (v < lo) lo = v; if (v > hi) hi = v;
        }
    }
    if (n < 16) { TdAppend(json, len, size, ", \"heights\": null"); return; }
    double mean = sum / n, sd = sqrt(fmax(0.0, sum2 / n - mean * mean));
    static uint32_t hist[1024];
    memset(hist, 0, sizeof hist);
    double span = hi - lo > 1e-9 ? hi - lo : 1e-9;
    for (uint32_t y = 0; y < h; y++)
    {
        double dy = y + 0.5 - cy;
        for (uint32_t x = 0; x < w; x++)
        {
            double dx = x + 0.5 - cx;
            if (dx * dx + dy * dy > r2) continue;
            double v = f[(size_t)y * w + x];
            if (!(v == v)) continue;
            int b = (int)((v - lo) / span * 1023.0); hist[b < 0 ? 0 : b > 1023 ? 1023 : b]++;
        }
    }
    double pct[3] = {0.01, 0.5, 0.99}, pv[3] = {lo, mean, hi};
    for (int k = 0; k < 3; k++)
    {
        size_t want = (size_t)(pct[k] * n), seen = 0;
        for (int b = 0; b < 1024; b++) { seen += hist[b]; if (seen > want) { pv[k] = lo + (b + 0.5) / 1024.0 * span; break; } }
    }
    /* slopes and curvature at full resolution */
    static const double edges[6] = {1, 2, 5, 10, 20, 35};
    double slopeBins[7] = {0}, slopeSum = 0, lapSum2 = 0; size_t sn = 0;
    for (uint32_t y = 1; y + 1 < h; y++)
    {
        double dy0 = y + 0.5 - cy;
        for (uint32_t x = 1; x + 1 < w; x++)
        {
            double dx0 = x + 0.5 - cx;
            if (dx0 * dx0 + dy0 * dy0 > r2) continue;
            size_t i = (size_t)y * w + x;
            double c = f[i], l = f[i - 1], rr = f[i + 1], u = f[i - w], dn = f[i + w];
            if (!(c == c && l == l && rr == rr && u == u && dn == dn)) continue;
            double gx = (rr - l) / (2 * metres), gy = (dn - u) / (2 * metres), deg = atan(sqrt(gx * gx + gy * gy)) * 57.29578;
            int b = 0; while (b < 6 && deg >= edges[b]) b++;
            slopeBins[b]++; slopeSum += deg;
            double lap = (l + rr + u + dn - 4 * c) / (metres * metres); lapSum2 += lap * lap; sn++;
        }
    }
    /* coarse grid: block means of f, at most 1024 per side */
    int factor = 1; while (w / factor > 1024 || h / factor > 1024) factor++;
    int d = (int)((w < h ? w : h) / factor);
    double cellM = metres * factor;
    for (int y = 0; y < d; y++)
        for (int x = 0; x < d; x++)
        {
            double s = 0; int c = 0;
            for (int yy = 0; yy < factor; yy++)
                for (int xx = 0; xx < factor; xx++)
                {
                    double v = f[(size_t)(y * factor + yy) * w + (x * factor + xx)];
                    if (v == v) { s += v; c++; }
                }
            g_tdCoarse[(size_t)y * d + x] = c ? (float)(s / c) : (float)mean;
            double ddx = (x + 0.5) * factor - cx, ddy = (y + 0.5) * factor - cy;
            g_tdMask[(size_t)y * d + x] = ddx * ddx + ddy * ddy <= r2;
        }
    for (int x = 0; x <= d; x++) g_tdSum[x] = 0;
    for (int y = 1; y <= d; y++)
    {
        double row = 0; g_tdSum[(size_t)y * (d + 1)] = 0;
        for (int x = 1; x <= d; x++) { row += g_tdCoarse[(size_t)(y - 1) * d + (x - 1)]; g_tdSum[(size_t)y * (d + 1) + x] = g_tdSum[(size_t)(y - 1) * (d + 1) + x] + row; }
    }
    static const double scales[5] = {8, 32, 128, 512, 2048};
    double detail[5];
    for (int s = 0; s < 5; s++)
    {
        int k = (int)(scales[s] / cellM + 0.5);
        if (k < 1 || 2 * k + 1 > d) { detail[s] = NAN; continue; }
        double a = 0, a2 = 0; size_t c = 0;
        for (int y = 0; y < d; y++)
            for (int x = 0; x < d; x++)
            {
                size_t i = (size_t)y * d + x;
                if (!g_tdMask[i]) continue;
                double e = g_tdCoarse[i] - TdBox(d, x, y, k); a += e; a2 += e * e; c++;
            }
        detail[s] = c ? sqrt(fmax(0.0, a2 / c - (a / c) * (a / c))) : NAN;
    }
    /* local relief: max - min in non-overlapping blocks (full resolution) */
    static const double blocks[3] = {32, 128, 512};
    double relief[3], reliefP90[3];
    static float ranges[1 << 20];
    for (int s = 0; s < 3; s++)
    {
        uint32_t bsz = (uint32_t)(blocks[s] / metres + 0.5);
        relief[s] = reliefP90[s] = NAN;
        if (bsz < 2 || bsz > w / 2) continue;
        uint32_t count = 0;
        for (uint32_t by = 0; by + bsz <= h && count < (1u << 20); by += bsz)
            for (uint32_t bx = 0; bx + bsz <= w && count < (1u << 20); bx += bsz)
            {
                double ddx = bx + bsz * 0.5 - cx, ddy = by + bsz * 0.5 - cy;
                if (ddx * ddx + ddy * ddy > r2) continue;
                float bl = 3e38f, bh = -3e38f;
                for (uint32_t y = by; y < by + bsz; y++)
                    for (uint32_t x = bx; x < bx + bsz; x++) { float v = f[(size_t)y * w + x]; if (v == v) { if (v < bl) bl = v; if (v > bh) bh = v; } }
                if (bh >= bl) ranges[count++] = bh - bl;
            }
        if (!count) continue;
        double t = 0; for (uint32_t k = 0; k < count; k++) t += ranges[k];
        relief[s] = t / count;
        qsort(ranges, count, sizeof ranges[0], TdFloatCompare);
        reliefP90[s] = ranges[(uint32_t)(count * 0.9) < count ? (uint32_t)(count * 0.9) : count - 1];
    }
    /* correlation length after removing the 512 m (and, on worlds of 8 km or more, 2048 m) local mean */
    static float detr[1024 * 1024];
    double corr[2][2] = {{NAN, NAN}, {NAN, NAN}};
    static const double trends[2] = {512, 2048};
    for (int t = 0; t < 2; t++)
    {
        int k = (int)(trends[t] / cellM + 0.5);
        if (k < 2 || 2 * k + 1 > d || (t == 1 && cellM * d < 8000)) continue;
        double v2 = 0; size_t c = 0;
        for (int y = 0; y < d; y++)
            for (int x = 0; x < d; x++)
            {
                size_t i = (size_t)y * d + x;
                detr[i] = (float)(g_tdCoarse[i] - TdBox(d, x, y, k));
                if (g_tdMask[i]) { v2 += (double)detr[i] * detr[i]; c++; }
            }
        double var = c ? v2 / c : 0;
        corr[t][0] = TdCorrelation(d, cellM, detr, var, 1);
        corr[t][1] = TdCorrelation(d, cellM, detr, var, 0);
    }
    char s0[32], s1[32], s2[32], s3[32], s4[32], s5[32], s6[32], s7[32], s8[32], s9[32], s10[32], s11[32], s12[32], s13[32], s14[32], s15[32], s16[32], s17[32], s18[32];
    double flat = sn ? (slopeBins[0]) / sn : NAN, steep = sn ? slopeBins[6] / sn : NAN;
    TdAppend(json, len, size,
        ", \"metres_per_sample\": %g, \"disk_m\": %g, \"in_disk_samples\": %zu, \"heights\": {\"min\": %g, \"max\": %g, \"mean\": %g, \"sd\": %g, "
        "\"p01\": %g, \"p50\": %g, \"p99\": %g, \"p99_p01\": %g}, \"slope\": {\"mean_deg\": %s, \"flat_lt1\": %s, \"steep_ge35\": %s, \"bins\": "
        "[%g, %g, %g, %g, %g, %g, %g], \"bin_edges_deg\": [1, 2, 5, 10, 20, 35]}, \"curvature_sd_per_m\": %s, "
        "\"detail_sd_m\": {\"8\": %s, \"32\": %s, \"128\": %s, \"512\": %s, \"2048\": %s}, \"relief_m\": {\"32\": %s, \"128\": %s, \"512\": %s}, "
        "\"relief_p90_m\": {\"32\": %s, \"128\": %s, \"512\": %s}, \"correlation_m\": {\"x_512\": %s, \"y_512\": %s, \"x_2048\": %s, \"y_2048\": %s}, "
        "\"coarse_cell_m\": %g",
        metres, diskM, n, lo, hi, mean, sd, pv[0], pv[1], pv[2], pv[2] - pv[0], TdNum(s0, 32, sn ? slopeSum / sn : NAN), TdNum(s1, 32, flat),
        TdNum(s2, 32, steep), sn ? slopeBins[0] / sn : 0, sn ? slopeBins[1] / sn : 0, sn ? slopeBins[2] / sn : 0, sn ? slopeBins[3] / sn : 0,
        sn ? slopeBins[4] / sn : 0, sn ? slopeBins[5] / sn : 0, sn ? slopeBins[6] / sn : 0, TdNum(s3, 32, sn ? sqrt(lapSum2 / sn) : NAN),
        TdNum(s4, 32, detail[0]), TdNum(s5, 32, detail[1]), TdNum(s6, 32, detail[2]), TdNum(s7, 32, detail[3]), TdNum(s8, 32, detail[4]),
        TdNum(s9, 32, relief[0]), TdNum(s10, 32, relief[1]), TdNum(s11, 32, relief[2]), TdNum(s12, 32, reliefP90[0]), TdNum(s13, 32, reliefP90[1]),
        TdNum(s14, 32, reliefP90[2]), TdLength(s15, 32, corr[0][0]), TdLength(s16, 32, corr[0][1]), TdLength(s17, 32, corr[1][0]), TdLength(s18, 32, corr[1][1]), cellM);
    if (key)
    {
        key->set = 1; key->metresPerSample = metres; key->sd = sd; key->flat = flat; key->detail32 = detail[1]; key->detail128 = detail[2];
        key->corrX = corr[0][0]; key->corrY = corr[0][1]; key->slopeMean = sn ? slopeSum / sn : NAN;
    }
}

/* The scatter id table (game+0x3328F60: pointer, count) for ground ids. */
static uint32_t TdIdTable(uint32_t* out, uint32_t cap)
{
    uint64_t at = 0; uint32_t count = 0;
    uintptr_t g = g_world.game + 0x3328F60u;
    if (g_world.game == 0 || SafeRead(g, &at, 8) || SafeRead(g + 8, &count, 4) || count == 0 || count > cap || SafeRead(at, out, count * 4u)) return 0;
    return count;
}

/* The material mix (u32: weights in bits 0-7 and 8-15, slots at 16/21/26) or the ground ids (density 0-6, weights 7-11
 * and 12-16, ids at 17/22/27). Rows are flipped (row = h - 1 - v); the disk is symmetric, so it does not matter here. */
static void TdPacked(const uint32_t* p, uint32_t w, uint32_t h, double diskM, double metres, int ground, char* json, int* len, int size)
{
    double cx = w * 0.5, cy = h * 0.5, r = diskM > 0 ? diskM / metres : w * 0.5, r2 = r * r;
    double cover[32] = {0}, wa = 0, wb = 0, painted = 0, density = 0; size_t n = 0;
    for (uint32_t y = 0; y < h; y++)
    {
        double dy = y + 0.5 - cy;
        for (uint32_t x = 0; x < w; x++)
        {
            double dx = x + 0.5 - cx;
            if (dx * dx + dy * dy > r2) continue;
            uint32_t v = p[(size_t)y * w + x];
            n++;
            if (!ground)
            {
                double a = v & 0xFF, b = (v >> 8) & 0xFF, rest = 255.0 - a - b; if (rest < 0) rest = 0;
                wa += a; wb += b; if (a + b > 0) painted++;
                cover[(v >> 16) & 31] += a / 255.0; cover[(v >> 21) & 31] += b / 255.0; cover[(v >> 26) & 31] += rest / 255.0;
            }
            else
            {
                double w1 = (v >> 7) & 31, w2 = (v >> 12) & 31;
                density += v & 0x7F;
                /* the callback picks id 2 for a random 0-31 at or below w2, id 1 at or below w1 + w2, else id 0 */
                double p2 = w2 ? (w2 + 1) / 32.0 : 0, p1 = w1 ? fmin(1.0 - p2, (w1 + w2 + 1) / 32.0 - p2) : 0, p0 = 1.0 - p2 - (p1 > 0 ? p1 : 0);
                cover[(v >> 27) & 31] += p2; cover[(v >> 22) & 31] += p1 > 0 ? p1 : 0; cover[(v >> 17) & 31] += p0 > 0 ? p0 : 0;
            }
        }
    }
    if (!n) return;
    if (!ground)
    {
        TdAppend(json, len, size, ", \"in_disk_samples\": %zu, \"painted_share\": %g, \"weight_a_mean\": %g, \"weight_b_mean\": %g, \"slot_cover\": {",
            n, painted / n, wa / n, wb / n);
        g_td.materialPainted = painted / n; g_td.materialSeen = 1;
    }
    else
    {
        uint32_t table[64]; uint32_t count = TdIdTable(table, 64);
        TdAppend(json, len, size, ", \"in_disk_samples\": %zu, \"density_mean\": %g, \"id_table\": [", n, density / n);
        for (uint32_t k = 0; k < count; k++) TdAppend(json, len, size, "%s\"0x%X\"", k ? ", " : "", table[k]);
        TdAppend(json, len, size, "], \"id_cover\": {");
        g_td.groundDensity = density / n; g_td.groundSeen = 1;
    }
    int first = 1;
    for (int k = 0; k < 32; k++) if (cover[k] / n >= 0.0005) { TdAppend(json, len, size, "%s\"%d\": %.4f", first ? "" : ", ", k, cover[k] / n); first = 0; }
    TdAppend(json, len, size, "}");
}

/* Any other image: value spread (as floats if they look like floats, else as integers) and distinct values. */
static int TdLooksFloat(const uint32_t* p, size_t samples)
{
    size_t finite = 0, step = samples > 65536 ? samples / 65536 : 1, seen = 0;
    for (size_t i = 0; i < samples; i += step)
    {
        float v; memcpy(&v, &p[i], 4); seen++;
        if (v == v && fabsf(v) < 1e5f && (fabsf(v) > 1e-20f || v == 0.0f)) finite++;
    }
    return seen && finite >= seen * 99 / 100;
}

static void TdDistinct(const uint32_t* p, size_t samples, char* json, int* len, int size)
{
    static uint32_t values[64]; static double counts[64];
    uint32_t distinct = 0, overflow = 0;
    size_t step = samples > (1u << 20) ? samples / (1u << 20) : 1, seen = 0;
    for (size_t i = 0; i < samples; i += step)
    {
        uint32_t v = p[i], k = 0; seen++;
        for (; k < distinct; k++) if (values[k] == v) { counts[k]++; break; }
        if (k == distinct) { if (distinct < 64) { values[distinct] = v; counts[distinct++] = 1; } else overflow = 1; }
    }
    TdAppend(json, len, size, ", \"distinct\": %s%u, \"top\": {", overflow ? "\"64+\", \"distinct_counted\": " : "", distinct);
    for (uint32_t k = 0; k < distinct && k < 12; k++) TdAppend(json, len, size, "%s\"0x%08X\": %.4f", k ? ", " : "", values[k], counts[k] / seen);
    TdAppend(json, len, size, "}");
}

static void TdDump(const char* label, const void* data, uint32_t w, uint32_t h, uint32_t bpp, const char* kind, double metres, int flipped)
{
    wchar_t name[96];
    swprintf(name, 96, L"terrain-%ld-%02u-%hs.bin", (long)g_td.level, g_td.images, label);
    HANDLE file = OpenLog(name);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    uint64_t bytes = (uint64_t)w * h * bpp, at = 0;
    while (at < bytes) { DWORD chunk = (DWORD)(bytes - at > 0x40000000u ? 0x40000000u : bytes - at); if (!WriteFile(file, (const uint8_t*)data + at, chunk, &wrote, NULL)) break; at += wrote; }
    CloseHandle(file);
    swprintf(name, 96, L"terrain-%ld-%02u-%hs.txt", (long)g_td.level, g_td.images, label);
    file = OpenLog(name);
    if (file == INVALID_HANDLE_VALUE) return;
    char text[400];
    int n = snprintf(text, sizeof text, "label %s\nkind %s\ncolumns %u\nrows %u\nbytes_per_sample %u\nmetres_per_sample %g\nworld_m %g\ndisk_m %g\nrows_flipped %d\nworld_scale %g\n",
        label, kind, w, h, bpp, metres, g_td.world, g_td.disk, flipped, (double)g_world.current);
    WriteFile(file, text, (DWORD)n, &wrote, NULL);
    CloseHandle(file);
}

/* The number after key in text (NaN if missing or null). */
static double TdField(const char* text, const char* key)
{
    const char* at = strstr(text, key);
    if (!at) return NAN;
    char* end; double v = strtod(at + strlen(key), &end);
    return end == at + strlen(key) ? NAN : v;
}

/* Measure one image and add it to the report. data is readable (copied or ours); bpp 2 or 4. */
static void TdImage(const char* label, const char* source, uint32_t afterOp, uint32_t record, TdKind kind, const void* data, uint32_t w, uint32_t h,
    uint32_t bpp, double metres, int flipped, TdKey* key)
{
    if (data == NULL || w < 16 || h < 16 || w > 16384 || h > 16384) return;
    char* json = g_td.imagesJson; int* len = &g_td.imagesLen; int size = sizeof g_td.imagesJson;
    static const char* const kinds[] = {"heights_u16", "heights_f32", "materials", "ground", "zones", "raw"};
    size_t samples = (size_t)w * h;
    if (kind == TD_RAW && bpp == 4 && TdLooksFloat((const uint32_t*)data, samples)) kind = TD_HEIGHTS_F32;
    LARGE_INTEGER t0, t1, hz; QueryPerformanceCounter(&t0);
    TdAppend(json, len, size, "%s\n    {\"label\": \"%s\", \"source\": \"%s\", \"after_op\": \"0x%02X\", \"after_pass\": \"%s\", \"record\": %u, \"kind\": \"%s\", "
        "\"columns\": %u, \"rows\": %u, \"bytes_per_sample\": %u",
        g_td.images ? "," : "", label, source, afterOp, TdOpName(afterOp), record, kinds[kind], w, h, bpp);
    int before = *len;
    if ((kind == TD_HEIGHTS_U16 || kind == TD_HEIGHTS_F32) && TdReserve(samples))
    {
        if (kind == TD_HEIGHTS_U16) { const uint16_t* s = (const uint16_t*)data; for (size_t i = 0; i < samples; i++) g_tdField[i] = s[i] / 65535.0f * 1000.0f - 500.0f; }
        else memcpy(g_tdField, data, samples * 4);
        TdHeights(w, h, metres, g_td.disk, json, len, size, key);
    }
    else if (kind == TD_MATERIALS || kind == TD_GROUND) TdPacked((const uint32_t*)data, w, h, g_td.disk, metres, kind == TD_GROUND, json, len, size);
    else if (bpp == 4) TdDistinct((const uint32_t*)data, samples, json, len, size);
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&hz);
    TdAppend(json, len, size, ", \"measure_ms\": %.1f}", (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart);
    if (g_td.mode >= 2) TdDump(label, data, w, h, bpp, kinds[kind], metres, flipped);
    g_td.images++;
    /* a one-line summary in modloader.log, from the JSON just written */
    char summary[300] = "";
    double sd = TdField(json + before, "\"sd\": "), flat = TdField(json + before, "\"flat_lt1\": "), d32 = TdField(json + before, "\"detail_sd_m\": {\"8\": ");
    double r128 = TdField(json + before, "\"relief_m\": {\"32\": "), cx = TdField(json + before, "\"x_512\": "), cy = TdField(json + before, "\"y_512\": ");
    double painted = TdField(json + before, "\"painted_share\": "), dens = TdField(json + before, "\"density_mean\": "), slope = TdField(json + before, "\"mean_deg\": ");
    if (sd == sd) snprintf(summary, sizeof summary, "sd %.1f m, slope %.1f deg, flat %.1f%%, detail@8m %.2f m, relief@32m %.2f m, feature length x/y %.0f/%.0f m",
        sd, slope, 100 * flat, d32, r128, cx, cy);
    else if (painted == painted) snprintf(summary, sizeof summary, "painted %.2f%%", 100 * painted);
    else if (dens == dens) snprintf(summary, sizeof summary, "density %.1f of 127", dens);
    Note("terrain diag (level %ld, %.0fx): %s (%s, after 0x%02X %s): %u x %u at %.2f m: %s", (long)g_td.level, (double)g_world.current, label, source,
        afterOp, TdOpName(afterOp), w, h, metres, summary);
}

/* Measure an image the game holds (descriptor {w, h, format, ?, data}); an image whose size, place and sampled content
 * were already measured this level is skipped. */
static void TdDescriptor(const char* label, const char* source, uint32_t afterOp, uint32_t record, TdKind kind, uintptr_t descriptor, double metres,
    int flipped, TdKey* key)
{
    uint32_t wh[4] = {0}; uint64_t data = 0;
    if (SafeRead(descriptor, wh, 16) || SafeRead(descriptor + 0x10, &data, 8) || data < 0x10000 || wh[0] < 16 || wh[1] < 16 || wh[0] > 16384 || wh[1] > 16384) return;
    uint32_t bpp = wh[2] == 0x16 ? 2 : 4;
    if (kind == TD_HEIGHTS_U16 && bpp != 2) kind = TD_RAW;
    if (metres <= 0) metres = g_td.world / wh[0];
    size_t bytes = (size_t)wh[0] * wh[1] * bpp;
    void* copy = VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (copy == NULL) return;
    if (!SafeRead((uintptr_t)data, copy, bytes))
    {
        uint64_t hash = (0xCBF29CE484222325ull ^ data ^ ((uint64_t)wh[0] << 32 | wh[1])) * 0x100000001B3ull;
        for (uint32_t y = 0; y < 64; y++)
            for (uint32_t x = 0; x < 64; x++)
            {
                uint32_t v = 0; memcpy(&v, (const uint8_t*)copy + ((size_t)(y * wh[1] / 64) * wh[0] + x * wh[0] / 64) * bpp, bpp);
                hash = (hash ^ v) * 0x100000001B3ull;
            }
        int seen = 0;
        for (uint32_t k = 0; k < g_td.lastDataCount; k++) if (g_td.lastData[k] == hash) seen = 1;
        if (!seen)
        {
            if (g_td.lastDataCount < 64) g_td.lastData[g_td.lastDataCount++] = hash;
            TdImage(label, source, afterOp, record, kind, copy, wh[0], wh[1], bpp, metres, flipped, key);
        }
    }
    VirtualFree(copy, 0, MEM_RELEASE);
}

/* ---- readbacks of the named targets ------------------------------------------------------------------------------ */

typedef uint64_t (*TdFindFn)(uint32_t name);
typedef uint32_t (*TdRequestFn)(uint64_t target);
typedef uint8_t (*TdReadyFn)(uint32_t id);
typedef void* (*TdGetFn)(void* out, uint32_t id);

static uint64_t TdRenderTable(void)
{
    uint64_t engine = 0, table = 0;
    if (SafeRead(g_world.game + 0x3326308u, &engine, 8) || engine == 0 || SafeRead(engine + 0x10, &table, 8)) return 0;
    return table;
}

static void TdRequest(uint32_t hash, TdKind kind, const char* name, uint32_t op, uint32_t record)
{
    if (!g_td.readback || g_td.pendingCount >= TD_MAX_PENDING || g_td.readbacks >= 14 || g_td.sessionReadbacks >= 48) return;
    uint64_t table = TdRenderTable(), find = 0, request = 0;
    if (table == 0 || SafeRead(table + 0x158, &find, 8) || SafeRead(table + 0x1A8, &request, 8) || !find || !request) return;
    uint64_t target = ((TdFindFn)(uintptr_t)find)(hash);
    if (target == 0) { Note("terrain diag (level %ld): no %s target after 0x%02X", (long)g_td.level, name, op); return; }
    uint32_t id = ((TdRequestFn)(uintptr_t)request)(target);
    if (id == 0xFFFFFFFFu) return;
    TdPending* p = &g_td.pending[g_td.pendingCount++];
    p->id = id; p->hash = hash; p->record = record; p->op = op; p->kind = kind; p->since = GetTickCount64();
    snprintf(p->label, sizeof p->label, "%s-after-%02X", name, op);
    g_td.readbacks++; g_td.sessionReadbacks++;
}

static void TdCollect(void)
{
    if (!g_td.pendingCount) return;
    uint64_t table = TdRenderTable(), ready = 0, get = 0;
    if (table == 0 || SafeRead(table + 0x1D0, &ready, 8) || SafeRead(table + 0x1D8, &get, 8) || !ready || !get) return;
    for (uint32_t k = 0; k < g_td.pendingCount;)
    {
        TdPending* p = &g_td.pending[k];
        int done = 0;
        if (((TdReadyFn)(uintptr_t)ready)(p->id))
        {
            uint8_t out[64] = {0};
            void* descriptor = ((TdGetFn)(uintptr_t)get)(out, p->id);
            if (descriptor) TdDescriptor(p->label, "readback", p->op, p->record, p->kind, (uintptr_t)descriptor, 0, 1, NULL);
            done = 1;
        }
        else if (GetTickCount64() - p->since > 20000) { Note("terrain diag (level %ld): readback %s never came", (long)g_td.level, p->label); done = 1; }
        if (done) g_td.pending[k] = g_td.pending[--g_td.pendingCount]; else k++;
    }
}

/* Which targets to read back once a pass's last step returns: only while the merge's targets exist (after
 * SetupTerrainMerge, before CleanupTerrainMerge releases them), the zone lookup once RenderZoneLookup drew it. */
static void TdScheduleReadbacks(uint32_t op, uint32_t record)
{
    if (!g_td.merging) return;
    switch (op)
    {
    case 0x3C: case 0x40: case 0x3E: case 0x3F: case 0x3D: case 0x0D: case 0x63:
        TdRequest(TD_RT_HEIGHTS, TD_HEIGHTS_U16, "heights", op, record);
        break;
    case 0x41:
        TdRequest(TD_RT_MATERIALS, TD_MATERIALS, "materials", op, record);
        break;
    case 0x4C:
        TdRequest(TD_RT_ZONES, TD_ZONES, "zones", op, record);
        break;
    case 0x64:
        TdRequest(TD_RT_HEIGHTS, TD_HEIGHTS_U16, "heights", op, record);
        TdRequest(TD_RT_MATERIALS, TD_MATERIALS, "materials", op, record);
        break;
    }
}

/* ---- after a pass ------------------------------------------------------------------------------------------------ */

static void TdPassDone(uint32_t op, uint32_t record, uint64_t recordAt)
{
    uintptr_t gen = TdGen();
    if (op == 0x38) { g_td.merging = 1; TdSettings(recordAt); } /* again: the merge's tiles are set during 0x38 */
    if (op == 0x66) g_td.merging = 0;
    if (op == 0x5C && gen != 0)
    {
        g_td.captureSets++;
        for (int k = 0; k < 8; k++)
        {
            char label[48]; snprintf(label, sizeof label, "capture%u-%d", g_td.captureSets, k);
            TdDescriptor(label, "merge capture", op, record, TD_RAW, gen + 0x119FF68u + 0x18u * (uintptr_t)k, 0, 1, NULL);
        }
    }
    if (op == 0x66 && gen != 0)
    {
        TdDescriptor("scatter-heights", "scatter image (lg+0xD8)", op, record, TD_HEIGHTS_U16, gen + TD_IMAGE_BLOCK + 0xD8, 0, 1, &g_td.imageHeights);
        TdDescriptor("scatter-materials", "scatter image (lg+0xF0)", op, record, TD_MATERIALS, gen + TD_IMAGE_BLOCK + 0xF0, 0, 1, NULL);
    }
    if (op == 0xBB)
    {
        /* the ground ids (made by op 0x47) once this pass has filled the scatter's id table they index */
        if (gen != 0) TdDescriptor("scatter-ground", "scatter image (lg+0x108)", op, record, TD_GROUND, gen + TD_IMAGE_BLOCK + 0x108, 0, 1, NULL);
        float area = 0;
        if (!SafeRead(g_world.game + 0x3328F6Cu, &area, 4) && area > 0) g_td.scatterRadius = sqrt(area);
    }
    if (op == 0x6D && g_heightCopy != NULL && g_heightBytes != 0)
    {
        uint32_t dims[2]; float extent[2];
        memcpy(dims, g_heightField + 0x1C, 8); memcpy(extent, g_heightField + 0x14, 8);
        if (dims[0] >= 16 && (uint64_t)dims[0] * dims[1] * 4u == g_heightBytes && extent[0] > 0)
            TdImage("layer-heightfield", "layer heightfield (kept copy)", op, record, TD_HEIGHTS_F32, g_heightCopy, dims[0], dims[1], 4,
                extent[0] / dims[0], 0, &g_td.finalHeights);
    }
    if (g_td.mode >= 2) TdScheduleReadbacks(op, record);
}

/* ---- the report --------------------------------------------------------------------------------------------------- */

static void TdFlag(const char* code, const char* format, ...)
{
    char text[300];
    va_list arguments; va_start(arguments, format); vsnprintf(text, sizeof text, format, arguments); va_end(arguments);
    for (char* c = text; *c; c++) if (*c == '"' || *c == '\\') *c = '\'';
    TdAppend(g_td.flags, &g_td.flagsLen, sizeof g_td.flags, "%s\n    {\"code\": \"%s\", \"text\": \"%s\"}", g_td.flagsLen ? "," : "", code, text);
    Note("terrain diag (level %ld, %.0fx): FLAG %s: %s", (long)g_td.level, (double)g_world.current, code, text);
}

static void TdFlags(void)
{
    const TdKey* k = g_td.finalHeights.set ? &g_td.finalHeights : g_td.imageHeights.set ? &g_td.imageHeights : NULL;
    if (k)
    {
        if (k->metresPerSample > 1.0) TdFlag("resolution", "the terrain has %.2f m per sample (vanilla 0.5): detail finer than %.1f m cannot exist", k->metresPerSample, 2 * k->metresPerSample);
        if (k->flat == k->flat && k->flat > 0.6) TdFlag("flat", "%.0f%% of the playable disk is flatter than 1 degree", 100 * k->flat);
        if (k->detail32 == k->detail32 && k->detail32 < 0.3) TdFlag("smooth", "height detail at 32 m is %.2f m (sd of the height less its 32 m mean)", k->detail32);
        else if (k->detail32 != k->detail32) TdFlag("smooth", "no detail at 32 m can be measured at %.2f m per sample", k->metresPerSample);
        if (k->corrX == k->corrX && k->corrY == k->corrY)
        {
            double x = fabs(k->corrX), y = fabs(k->corrY); /* negative: longer than the search, at least |v| */
            if (k->corrX < 0 && k->corrY < 0) TdFlag("long-features", "features along both axes are longer than %.0f m once the 512 m trend is gone: no small-scale relief", x);
            else if (k->corrX < 0 || k->corrY < 0 || x / y > 1.33 || y / x > 1.33)
                TdFlag("anisotropy", "features are %s%.0f m long along x and %s%.0f m along y: uneven stretching", k->corrX < 0 ? "over " : "", x,
                    k->corrY < 0 ? "over " : "", y);
        }
    }
    else TdFlag("no-heights", "no heightfield was measured (the layer heightfield and the scatter heights were both missing)");
    if (g_td.planSpacing > 24.0) TdFlag("stretched-lattice", "the Voronoi lattice's median spacing is %.1f m (16 m at vanilla density): the plan's relief is %.1f x coarser", g_td.planSpacing, g_td.planSpacing / 16.0);
    if (g_td.materialSeen && g_td.materialPainted < 0.02) TdFlag("unpainted", "%.2f%% of the playable disk has a painted material: no grass or plant records are chosen", 100 * g_td.materialPainted);
    if (g_td.groundSeen && g_td.groundDensity < 32) TdFlag("sparse-scatter", "the ground ids' mean density is %.1f of 127", g_td.groundDensity);
    if (g_td.scatterRadius > 0 && g_td.disk > 0 && g_td.scatterRadius < g_td.disk - 1) TdFlag("scatter-cutoff", "scatter stops %.0f m from the centre, inside the %.0f m disk", g_td.scatterRadius, g_td.disk);
    char zone[8] = ""; GetEnvironmentVariableA("HELLFORGE_XL_ZONE_REPEAT", zone, sizeof zone);
    if (g_td.world > 2100 && zone[0] != '1') TdFlag("zone-materials-stretched", "zone height materials span the whole %.0f m world once (%.1f x vanilla's texel size)", g_td.world, g_td.world / 2048.0);
    if (g_td.pendingCount) TdFlag("readbacks-lost", "%u readbacks never came back", g_td.pendingCount);
}

static void TdReport(void)
{
    if (g_td.reported || g_td.level < 0) return;
    g_td.reported = 1;
    if (!g_td.settingsLen) TdSettings(0);
    TdFlags();
    wchar_t name[64];
    swprintf(name, 64, L"terrain-diag-%ld.json", (long)g_td.level);
    HANDLE file = OpenLog(name);
    if (file == INVALID_HANDLE_VALUE) return;
    static char head[1024], ops[96 * 1024];
    DWORD wrote;
    int hl = 0, ol = 0;
    TdAppend(head, &hl, sizeof head, "{\n  \"format\": 1,\n  \"level\": %ld,\n  \"world_scale\": %g,\n  \"world_m\": %g,\n  \"disk_m\": %g,\n  \"scatter_radius_m\": %g,\n  \"records\": %u,\n  \"frames\": %u,\n  \"settings\": ",
        (long)g_td.level, (double)g_world.current, g_td.world, g_td.disk, g_td.scatterRadius, g_td.records, g_td.frames);
    WriteFile(file, head, (DWORD)hl, &wrote, NULL);
    WriteFile(file, g_td.settingsLen ? g_td.settings : "null", (DWORD)(g_td.settingsLen ? g_td.settingsLen : 4), &wrote, NULL);
    WriteFile(file, ",\n  \"plan\": ", 12, &wrote, NULL);
    WriteFile(file, g_td.planLen ? g_td.plan : "null", (DWORD)(g_td.planLen ? g_td.planLen : 4), &wrote, NULL);
    TdAppend(ops, &ol, sizeof ops, ",\n  \"ops\": [");
    ULONGLONG start = 0;
    for (uint32_t i = 0; i < g_td.records && i < TD_MAX_RECORDS; i++) if (g_td.ops[i].steps && (!start || g_td.ops[i].first < start)) start = g_td.ops[i].first;
    int first = 1;
    for (uint32_t i = 0; i < g_td.records && i < TD_MAX_RECORDS; i++)
    {
        const TdOp* o = &g_td.ops[i];
        if (!o->steps) continue;
        TdAppend(ops, &ol, sizeof ops, "%s\n    {\"record\": %u, \"op\": \"0x%02X\", \"pass\": \"%s\", \"steps\": %u, \"ms\": %.2f, \"start_ms\": %llu, \"span_ms\": %llu, \"locations\": %d, \"done\": %s}",
            first ? "" : ",", i, o->op, TdOpName(o->op), o->steps, o->ms, (unsigned long long)(o->first - start), (unsigned long long)(o->last - o->first),
            (int)o->locAfter - (int)o->locBefore, o->done ? "true" : "false");
        first = 0;
    }
    TdAppend(ops, &ol, sizeof ops, "\n  ],\n  \"images\": [");
    WriteFile(file, ops, (DWORD)ol, &wrote, NULL);
    WriteFile(file, g_td.imagesJson, (DWORD)g_td.imagesLen, &wrote, NULL);
    WriteFile(file, "\n  ],\n  \"flags\": [", 18, &wrote, NULL);
    WriteFile(file, g_td.flags, (DWORD)g_td.flagsLen, &wrote, NULL);
    WriteFile(file, "\n  ]\n}\n", 7, &wrote, NULL);
    CloseHandle(file);
    Note("terrain diag (level %ld, %.0fx): wrote terrain-diag-%ld.json (%u passes, %u images, %u readbacks)", (long)g_td.level, (double)g_world.current,
        (long)g_td.level, g_td.records, g_td.images, g_td.readbacks);
}

/* ---- the hooks ------------------------------------------------------------------------------------------------------ */

static int TerrainDiagOn(void)
{
    if (g_td.mode < 0)
    {
        char text[16] = "";
        g_td.mode = GetEnvironmentVariableA("HELLFORGE_TERRAIN_DIAG", text, sizeof text) > 0 ? atoi(text) : 0;
        if (g_td.mode < 0 || g_td.mode > 2) g_td.mode = 0;
        char rb[16] = "";
        g_td.readback = g_td.mode >= 2 && !(GetEnvironmentVariableA("HELLFORGE_TERRAIN_DIAG_READBACK", rb, sizeof rb) > 0 && !_stricmp(rb, "off"));
        if (g_td.mode) Note("terrain diag: on (level %d%s): a report per level in terrain-diag-<level>.json", g_td.mode, g_td.readback ? ", with readbacks" : "");
    }
    return g_td.mode > 0;
}

/* Before the dispatcher runs a step: the record and its op, the clock. */
static void TerrainDiagBefore(uint64_t records, uint64_t index, uint32_t end, uint32_t* at, uint32_t* op)
{
    *at = 0xFFFFFFFFu; *op = 0xFFFFFFFFu;
    if (!TerrainDiagOn() || g_worldLevel == 0) return;
    if (SafeRead(index, at, 4) || SafeRead(records + (uint64_t)*at * 0x19B0u, op, 4)) { *at = 0xFFFFFFFFu; return; }
    AcquireSRWLockExclusive(&g_td.lock);
    if (g_td.level != g_world.levels) { if (g_td.level >= 0 && !g_td.reported) TdReport(); TdLevelReset(); }
    if (end && end <= TD_MAX_RECORDS) { g_td.end = end; if (end > g_td.records) g_td.records = end; }
    if (*at < TD_MAX_RECORDS)
    {
        TdOp* o = &g_td.ops[*at];
        ULONGLONG now = GetTickCount64();
        if (!o->steps) { o->first = now; o->op = *op; uintptr_t gen = TdGen(); if (gen) SafeRead(gen + 0x11A7108u, &o->locBefore, 4); }
        o->last = now;
        if (*at + 1 > g_td.records) g_td.records = *at + 1;
    }
    /* the plan as the first merge begins (the drop suggestion's moment: the generation's resources are still alive) */
    if (*op == 0x38 && !g_td.planned) { g_td.planned = 1; TdSettings(records + (uint64_t)*at * 0x19B0u); TdPlan(); }
    TdCollect();
    ReleaseSRWLockExclusive(&g_td.lock);
    QueryPerformanceCounter(&g_td.stepStart);
}

/* After it: the step's time; a record whose index moved on is done. */
static void TerrainDiagAfter(uint64_t records, uint64_t index, uint32_t at, uint32_t op)
{
    if (at == 0xFFFFFFFFu || g_td.mode <= 0) return;
    LARGE_INTEGER now, hz; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&hz);
    uint32_t after = at;
    SafeRead(index, &after, 4);
    AcquireSRWLockExclusive(&g_td.lock);
    if (g_td.level == g_world.levels && at < TD_MAX_RECORDS)
    {
        TdOp* o = &g_td.ops[at];
        o->steps++;
        o->ms += (double)(now.QuadPart - g_td.stepStart.QuadPart) * 1000.0 / (double)hz.QuadPart;
        if (after != at && !o->done)
        {
            o->done = 1;
            uintptr_t gen = TdGen(); if (gen) SafeRead(gen + 0x11A7108u, &o->locAfter, 4);
            TdPassDone(op, at, records + (uint64_t)at * 0x19B0u);
            if (g_td.end && after >= g_td.end) { g_td.ended = 1; g_td.endedAt = GetTickCount64(); }
        }
    }
    ReleaseSRWLockExclusive(&g_td.lock);
}

/* Every game frame (packages.c): collect readbacks, count frames, write the report once generation is over. */
static void TerrainDiagTick(void)
{
    if (g_td.mode <= 0) return;
    AcquireSRWLockExclusive(&g_td.lock);
    if (g_td.level == g_world.levels && !g_td.reported)
    {
        g_td.frames++;
        TdCollect();
        if (g_td.ended && (g_td.pendingCount == 0 || GetTickCount64() - g_td.endedAt > 20000)) TdReport();
    }
    ReleaseSRWLockExclusive(&g_td.lock);
}
