/* Towns on an extra large map at a vanilla town's shape and count (the level's op 0x2B, PlaceStampWithWeightRule).
 *
 * Op 0x2B's jobs (0xF322F0, one per head cell of a merged group) place each picked rule's stamps over every member cell:
 * a grid around the member's site (origin [rbp-0x60] / xmm14 from cell+0 at 0xF3281C, used at 0xF33621 / 0xF3369E) sized
 * by the member's box (cell+0x88 / +0x90, read only at 0xF335B7 / 0xF335C2, as nx x ny at 0xF335CD..0xF335F9), and
 * stamps along the member's routes (0xF314A0, its only instance at 0xF31A1B). An extra large map's route cells are
 * about 1/s times wider than vanilla's, so a picked group spread a town over kilometres with ~1/s^2 the grid points;
 * the loader thinned every instance instead (difficulty_world.c's below[]), which left a few per cent scattered.
 *
 * Here, for the level's own record only (its pass, record + 8 = the job's [rbp+0x238], 0xF323E0 / 0xF34F02):
 * - stub A (0xF335B3) maps the member's site and box toward the head cell's site H ([rbp+0xC0], 0xF326BD):
 *   p' = H + o + (p - H) x s, so the group is laid at a vanilla town's size inside its head cell;
 * - stub B (0xF337A0) gives the side test 0xF31BD0, which judges a point against the member's own edges, the point
 *   mapped back (H + (p' - H - o) / s, in the free slot [rbp+0x168]);
 * - stub C (0xF31A1B) keeps a route stamp with probability s (a hash of its position), vanilla's count per member;
 * - the thinning is off for the run, and the record runs twice more (op 0x2B appends; its PCG at record+0x1990 moves
 *   on, 0xF34E54), each with its own offset o, for the three missions' share of towns: x3 a vanilla level's picks.
 * s = (681 / disk) x sqrt(N / 430): the route sites are a Poisson disc, so a vanilla group is the XL group scaled
 * (681 m and 430 cells are a vanilla level's disk and route cells; N this level's cells).
 * Level 11's rule and item filters (rule +0x58: max = (w >> 5) & 0xF at 0xF3275D; item +0x60: max = (w >> 6) & 0xF at
 * 0xF3526B) drop what vanilla gives level 10 at most; those are widened to 11 (harmless below 11).
 * Included by hellforge_modloader.c after difficulty_world_terrain.c. */

#define TOWN_GRID_SITE 0xF335B3u   /* 26 bytes, back 0xF335CD */
#define TOWN_SIDE_SITE 0xF337A0u   /* 12 bytes, back 0xF337AC */
#define TOWN_ROUTE_SITE 0xF31A1Bu  /* 9 bytes, back 0xF31A24, skip 0xF31AEA (the function's no-instance exit) */
#define TOWN_ROUTE_SKIP 0xF31AEAu
#define TOWN_DATA 0x1C0u           /* the stubs' values: pass +0, offset +8, s +0x10, 1/s +0x14, run +0x18, route keep +0x1C */
#define TOWN_RUNS 3
static const uint8_t kTownStubs[0x1C0] = {
    0x49, 0x8B, 0x4A, 0x18, 0x48, 0x8B, 0x81, 0x88, 0x00, 0x00, 0x00, 0x48, 0x89, 0x45, 0xF8, 0x48,
    0x8B, 0x81, 0x90, 0x00, 0x00, 0x00, 0x48, 0x89, 0x45, 0x18, 0x48, 0x8B, 0x85, 0x38, 0x02, 0x00,
    0x00, 0x48, 0x3B, 0x05, 0x98, 0x01, 0x00, 0x00, 0x75, 0x66, 0x48, 0x8B, 0x85, 0xC0, 0x00, 0x00,
    0x00, 0xF3, 0x0F, 0x7E, 0x00, 0xF3, 0x0F, 0x7E, 0x0D, 0x8B, 0x01, 0x00, 0x00, 0x0F, 0x58, 0xC8,
    0xF3, 0x0F, 0x10, 0x15, 0x88, 0x01, 0x00, 0x00, 0x0F, 0xC6, 0xD2, 0x00, 0xF3, 0x0F, 0x7E, 0x5D,
    0xF8, 0x0F, 0x5C, 0xD8, 0x0F, 0x59, 0xDA, 0x0F, 0x58, 0xD9, 0x66, 0x0F, 0xD6, 0x5D, 0xF8, 0xF3,
    0x0F, 0x7E, 0x5D, 0x18, 0x0F, 0x5C, 0xD8, 0x0F, 0x59, 0xDA, 0x0F, 0x58, 0xD9, 0x66, 0x0F, 0xD6,
    0x5D, 0x18, 0xF3, 0x0F, 0x7E, 0x19, 0x0F, 0x5C, 0xD8, 0x0F, 0x59, 0xDA, 0x0F, 0x58, 0xD9, 0x66,
    0x0F, 0xD6, 0x5D, 0xA0, 0x0F, 0x28, 0xE3, 0x0F, 0xC6, 0xE4, 0x55, 0xF3, 0x44, 0x0F, 0x10, 0xF4,
    0xE9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x66, 0x0F, 0x7F, 0x85, 0x50, 0x03, 0x00, 0x00, 0x4C, 0x8D, 0x4D, 0x90, 0x48, 0x8B, 0x85, 0x38,
    0x02, 0x00, 0x00, 0x48, 0x3B, 0x05, 0xE6, 0x00, 0x00, 0x00, 0x75, 0x3F, 0x48, 0x8B, 0x85, 0xC0,
    0x00, 0x00, 0x00, 0xF3, 0x0F, 0x7E, 0x08, 0xF3, 0x0F, 0x7E, 0x15, 0xD9, 0x00, 0x00, 0x00, 0x0F,
    0x58, 0xD1, 0xF3, 0x0F, 0x7E, 0x5D, 0x90, 0x0F, 0x5C, 0xDA, 0xF3, 0x0F, 0x10, 0x25, 0xD2, 0x00,
    0x00, 0x00, 0x0F, 0xC6, 0xE4, 0x00, 0x0F, 0x59, 0xDC, 0x0F, 0x58, 0xD9, 0x66, 0x0F, 0xD6, 0x9D,
    0x68, 0x01, 0x00, 0x00, 0x4C, 0x8D, 0x8D, 0x68, 0x01, 0x00, 0x00, 0xE9, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x8B, 0x84, 0x24, 0xA0, 0x02, 0x00, 0x00, 0x48, 0x8B, 0x80, 0x38, 0x02, 0x00, 0x00, 0x48,
    0x3B, 0x05, 0x6A, 0x00, 0x00, 0x00, 0x75, 0x31, 0x48, 0x89, 0xD8, 0x48, 0x89, 0xD9, 0x48, 0xC1,
    0xE9, 0x20, 0x69, 0xC0, 0xB1, 0x79, 0x37, 0x9E, 0x01, 0xC8, 0x69, 0xC0, 0x6B, 0xCA, 0xEB, 0x85,
    0x03, 0x05, 0x62, 0x00, 0x00, 0x00, 0x69, 0xC0, 0x35, 0xAE, 0xB2, 0xC2, 0x3B, 0x05, 0x5A, 0x00,
    0x00, 0x00, 0x76, 0x05, 0xE9, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x4C, 0x24, 0x68, 0x41, 0x0F,
    0x28, 0xD7, 0xE9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static struct
{
    uint8_t* block;
    volatile uint64_t* pass;
    volatile float* offset;     /* x, y */
    volatile float* s;          /* s, 1/s */
    volatile uint32_t* run;     /* run, route keep */
    float rho;                  /* the further runs' offset: a quarter of the level's route-cell spacing */
    LONG level;                 /* g_world.levels of the armed run */
    uint32_t before;            /* locations before the level's run */
    uint32_t saved[2];          /* below[] as the level had it */
    int armed;
} g_towns;

static const char* InstallTowns(uintptr_t game)
{
    char off[8] = "";
    if (GetEnvironmentVariableA("HELLFORGE_XL_TOWNS", off, sizeof off) > 0 && !_stricmp(off, "off")) return "towns at a vanilla town's shape are off (HELLFORGE_XL_TOWNS=off); the thinning stays";
    if (!ApiCodeMatches(game + TOWN_GRID_SITE, "49 8B 4A 18 48 8B 81 88 00 00 00 48 89 45 F8 48 8B 81 90 00 00 00 48 89 45 18") ||
        !ApiCodeMatches(game + TOWN_SIDE_SITE, "4C 8D 4D 90 66 0F 7F 85 50 03 00 00") ||
        !ApiCodeMatches(game + TOWN_ROUTE_SITE, "48 8B 4C 24 68 41 0F 28 D7") ||
        !ApiCodeMatches(game + 0xF323E0u, "4C 89 AD 38 02 00 00") || !ApiCodeMatches(game + 0xF326BDu, "48 89 B5 C0 00 00 00") ||
        !ApiCodeMatches(game + 0xF315ACu, "48 89 AC 24 A0 02 00 00") || !ApiCodeMatches(game + TOWN_ROUTE_SKIP, "44 0F 28 AC 24 10 02 00 00"))
        return "the stamp jobs are not the reviewed ones; towns keep the thinning";
    uint8_t* b = AllocateNearGame(game, 0x200);
    if (b == NULL) return "no memory within 2 GB of game.dll for the towns' stubs";
    memcpy(b, kTownStubs, sizeof kTownStubs);
    memset(b + TOWN_DATA, 0, 0x40);
    const struct { uint32_t at, end; uintptr_t to; } jumps[4] = {
        {0x91, 0x95, game + 0xF335CDu}, {0x11C, 0x120, game + 0xF337ACu}, {0x185, 0x189, game + TOWN_ROUTE_SKIP}, {0x193, 0x197, game + 0xF31A24u}};
    for (int i = 0; i < 4; i++) { int32_t rel = (int32_t)(jumps[i].to - (uintptr_t)(b + jumps[i].end)); memcpy(b + jumps[i].at, &rel, 4); }
    DWORD protect;
    VirtualProtect(b, 0x200, PAGE_EXECUTE_READWRITE, &protect);
    FlushInstructionCache(GetCurrentProcess(), b, 0x200);
    g_towns.block = b;
    g_towns.pass = (volatile uint64_t*)(b + TOWN_DATA);
    g_towns.offset = (volatile float*)(b + TOWN_DATA + 8);
    g_towns.s = (volatile float*)(b + TOWN_DATA + 0x10);
    g_towns.run = (volatile uint32_t*)(b + TOWN_DATA + 0x18);
    g_towns.level = -1;
    const struct { uint32_t site, length, stub; } sites[3] = {{TOWN_GRID_SITE, 26, 0}, {TOWN_SIDE_SITE, 12, 0xC0}, {TOWN_ROUTE_SITE, 9, 0x140}};
    for (int i = 0; i < 3; i++)
    {
        uint8_t patch[26];
        memset(patch, 0x90, sizeof patch);
        patch[0] = 0xE9;
        int32_t rel = DifficultyJumpRel(game + sites[i].site, (uintptr_t)(b + sites[i].stub)); memcpy(patch + 1, &rel, 4);
        if (ApiWrite(game + sites[i].site, patch, sites[i].length))
        {
            for (int k = 0; k < i; k++) /* put back the ones already written */
            {
                static const char* pins[2] = {"49 8B 4A 18 48 8B 81 88 00 00 00 48 89 45 F8 48 8B 81 90 00 00 00 48 89 45 18", "4C 8D 4D 90 66 0F 7F 85 50 03 00 00"};
                uint8_t bytes[26]; int n = 0;
                for (const char* p = pins[k]; *p; ) { while (*p == ' ') p++; if (!*p) break; bytes[n++] = (uint8_t)strtoul(p, (char**)&p, 16); }
                ApiWrite(game + sites[k].site, bytes, (size_t)n);
            }
            return "the towns' stubs could not be written";
        }
    }
    return NULL;
}

/* Level 11's filters: rules and items whose maximum level is 10 (vanilla's top) take 11 too. */
static void TownsWidenLevels(uint64_t record)
{
    static LONG done = -1;
    if (done == g_world.levels) return;
    done = g_world.levels;
    uint64_t table = 0; uint32_t types = 0, rules = 0, items = 0, unwritable = 0;
    if (SafeRead(record + 0x50, &table, 8) || SafeRead(record + 0x160, &types, 4) || table == 0 || types > 4096) return;
    for (uint32_t t = 0; t < types; t++)
    {
        uint64_t type = 0, list = 0; uint32_t n = 0;
        if (SafeRead(table + 8ull * t, &type, 8) || type == 0 || SafeRead(type + 0x10, &list, 8) || SafeRead(type + 0x18, &n, 4) || list == 0 || n > 256) continue;
        for (uint32_t r = 0; r < n; r++)
        {
            uint64_t rule = list + 0x70ull * r, itemList = 0; uint16_t w = 0; uint32_t m = 0;
            if (SafeRead(rule + 0x58, &w, 2)) continue;
            if ((w & 0x1E0) != 0x1E0 && ((w >> 5) & 0xF) == 10)
            {
                if (!WorldDataWritable(rule + 0x58, 2)) unwritable++;
                else { *(volatile uint16_t*)(uintptr_t)(rule + 0x58) = (uint16_t)((w & ~0x1E0u) | (11u << 5)); rules++; }
            }
            if (SafeRead(rule, &itemList, 8) || SafeRead(rule + 8, &m, 4) || itemList == 0 || m > 1024) continue;
            for (uint32_t i = 0; i < m; i++)
            {
                uint64_t at = itemList + 0x68ull * i + 0x60;
                if (SafeRead(at, &w, 2) || (w & 0x3C0) == 0x3C0 || ((w >> 6) & 0xF) != 10) continue;
                if (!WorldDataWritable(at, 2)) { unwritable++; continue; }
                *(volatile uint16_t*)(uintptr_t)at = (uint16_t)((w & ~0x3C0u) | (11u << 6)); items++;
            }
        }
    }
    Note("difficulty: towns: level 11 takes %u rules and %u stamps whose top level was 10 (%u types; %u not writable)", rules, items, types, unwritable);
}

/* Before each dispatcher call on the level's op 0x2B record (the op runs over several calls). */
static void TownsArm(uint64_t records, uint32_t at, uint64_t context, uintptr_t gen)
{
    if (g_towns.block == NULL || g_world.below == NULL) return;
    /* towns are the open maps' (types 0/1: weight-rule stamps on zone cells); a city map's buildings come from op 0xA7,
     * which on three cities fills the location table before 0x2B (s74f: 8192 at 0x2B, which appends without a check) */
    uint64_t settings0 = 0, info = 0; uint8_t mapType = 0xFF;
    if (SafeRead(context + 8, &settings0, 8) || settings0 == 0 || SafeRead(settings0 + 0xC0, &info, 8) || info == 0 || SafeRead(info + 0x1D, &mapType, 1) || mapType > 1)
    {
        static LONG noted = -1;
        if (noted != g_world.levels) { noted = g_world.levels; Note("difficulty: towns: map type %u keeps its op 0x2B as it is (towns are map types 0 and 1)", mapType); }
        return;
    }
    uint64_t record = records + (uint64_t)at * 0x19B0u;
    TownsWidenLevels(record);
    if (g_towns.level != g_world.levels)
    {
        uint64_t settings = 0; float keep = 0.0f; uint32_t cells = 0;
        if (SafeRead(context + 8, &settings, 8) || settings == 0 || SafeRead(settings + 0x6C, &keep, 4) || SafeRead(gen + 0x11A71C4u, &cells, 4) ||
            !(keep > 682.0f) || cells == 0)
            return;
        float s = (681.0f / (keep - 1.0f)) * sqrtf((float)cells / 430.0f);
        if (s < 1.0f / 20.0f) s = 1.0f / 20.0f;
        if (s > 1.0f / 8.0f) s = 1.0f / 8.0f;
        g_towns.level = g_world.levels;
        g_towns.s[0] = s; g_towns.s[1] = 1.0f / s;
        g_towns.run[1] = (uint32_t)(s * 4294967295.0f);
        g_towns.rho = 0.25f * (keep - 1.0f) * sqrtf(3.14159265f / (float)cells);
        g_towns.saved[0] = g_world.below[0]; g_towns.saved[1] = g_world.below[1];
        SafeRead(gen + 0x11A7108u, &g_towns.before, 4);
        Note("difficulty: towns: op 0x2B (record %u) at %.0fx: towns at 1/%.1f (s %.4f; %u route cells, disk %.0f m), %d runs, no thinning",
            at, (double)g_world.current, (double)(1.0f / s), (double)s, cells, (double)keep - 1.0, TOWN_RUNS);
    }
    g_towns.offset[0] = g_towns.offset[1] = 0.0f;
    g_towns.run[0] = 0;
    *g_towns.pass = record + 8;
    g_world.below[0] = g_world.below[1] = 0xFFFFFFFFu;
    g_towns.armed = 1;
}

/* After a dispatcher call on it: when the level's run is done, the further runs, then the thinning as it was. */
static void TownsAfter(uint64_t records, uint32_t at, uint64_t index, uint64_t context, uint64_t g, uint64_t h, uintptr_t gen)
{
    if (!g_towns.armed) return;
    uint32_t now = at;
    if (SafeRead(index, &now, 4) || now == at) return; /* still running */
    uint8_t* record = (uint8_t*)(uintptr_t)(records + (uint64_t)at * 0x19B0u);
    uint32_t counts[TOWN_RUNS + 1] = {g_towns.before, 0, 0, 0};
    SafeRead(gen + 0x11A7108u, &counts[1], 4);
    int runs = 1;
    const char* stop = NULL;
    for (int k = 1; k < TOWN_RUNS; k++)
    {
        uint32_t last = counts[k] - counts[k - 1];
        if (counts[k] + last + last / 2 >= DIF_GEN_LOCATIONS) { stop = "the location table would fill"; break; }
        uint64_t arena = 0;
        if (!SafeRead(context, &arena, 8) && arena != 0) ((void (*)(uint64_t))(g_world.game + 0x173BAB0u))(arena);
        const float rho = g_towns.rho, angle = 2.0943951f * (float)k;
        g_towns.offset[0] = rho * cosf(angle); g_towns.offset[1] = rho * sinf(angle);
        g_towns.run[0] = (uint32_t)k * 0x9E3779B9u;
        InterlockedIncrement(&g_worldOpRun);
        volatile uint32_t one = 0; volatile int calls = 0; uint8_t phase = 0;
        LONG fault = WorldTileGuarded(record, context, &one, &phase, g, h, 100000, &calls);
        InterlockedIncrement(&g_worldOpRun);
        if (fault) { stop = "a run faulted"; Note("difficulty: towns: run %d faulted (%08lX at game.dll+0x%llX)", k + 1, (unsigned long)fault, (unsigned long long)(TileGuardSelf()->at - g_world.game)); break; }
        if (one == 0) { stop = "a run did not finish"; break; }
        SafeRead(gen + 0x11A7108u, &counts[k + 1], 4);
        runs++;
    }
    *g_towns.pass = 0;
    g_towns.armed = 0;
    g_world.below[0] = g_towns.saved[0]; g_world.below[1] = g_towns.saved[1];
    Note("difficulty: towns: %d run(s): locations %u -> %u -> %u -> %u%s%s", runs, counts[0], counts[1], counts[2], counts[3], stop ? "; stopped: " : "", stop ? stop : "");
}

/* A level that is not extra large never runs with the stubs armed (a record at an armed run's address, the run left
 * unfinished). */
static void TownsForget(uint64_t pass)
{
    if (g_towns.block == NULL || *g_towns.pass != pass) return;
    *g_towns.pass = 0;
    if (g_towns.armed && g_world.below != NULL) { g_world.below[0] = g_towns.saved[0]; g_world.below[1] = g_towns.saved[1]; }
    g_towns.armed = 0;
}
