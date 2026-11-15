/* Terrain heights on level 11's larger maps (research d11-heightmaps §6, §8). Included by hellforge_modloader.c after
 * difficulty_towns.c.
 *
 * GenerateSubregionHeights (op 4, 0xF64E30) gives every area of a route cell the height of its cell type's curve at
 * t = clamp(d / 64 m), d the area's distance to the nearest route point (the 1/64 at 0xF651FA). A larger map keeps level
 * 10's number of route cells, 40 m apart becoming 65.75 m at 2 x, so routes are that much farther from everything and
 * far more ground lies at the curve's flat end (t = 1): flat terraces, and lake curves (down to 4-13 m below 0) at their
 * deepest under objectives. Objectives in a low cell take a corner height of exactly 0 (0xF3B957), so the area layer
 * alone sets their pad: an ICBM silo's lower platform, a few metres below its pad, could end under the region's water
 * (-0.25 to -4.5 m, region +0x44). On a larger map:
 *   - the ramp is 64 m x the cell factor (spacing / 40), so t follows level 10's distribution (off with
 *     HELLFORGE_D11_TERRAIN_RAMP=0, for comparing);
 *   - an area or vertex within an objective's stamp radius + 16 m never stores a height below 0 (the two stores 0xF655A9
 *     and 0xF656F9): with the corner layer >= 0, every pad is above every water level (off with
 *     HELLFORGE_D11_DRY_OBJECTIVES=0). The stamp radius is its location item's +0xD8, reached as GenerateHeightFromLocations
 *     does (0xF3BEE0..0xF3BF8E). The heights it raised are counted and DifficultyWatch logs them. */

#define DIF_TERRAIN_RAMP_SITE 0xF651FAu   /* movss xmm11, [rip+1/64] (9 bytes) */
#define DIF_TERRAIN_RAMP_BACK 0xF65203u
static const uint8_t kTerrainRampPin[9] = {0xF3, 0x44, 0x0F, 0x10, 0x1D, 0x45, 0x14, 0x46, 0x01};
static const uint8_t kTerrainRampStub[44] = {
    0xF3, 0x44, 0x0F, 0x10, 0x1D, 0, 0, 0, 0,                         /*  0 movss xmm11, [rip+1/64] (rel32 at 5) */
    0x49, 0x8B, 0x4D, 0x08,                                           /*  9 mov rcx, [r13+8] (lg+0x2D20) */
    0x81, 0x79, 0x5C, 0, 0, 0, 0,                                     /* 13 cmp dword [rcx+0x5c], the larger disk (at 16) */
    0x75, 0x09,                                                       /* 20 jne 31 */
    0xF3, 0x44, 0x0F, 0x10, 0x1D, 0, 0, 0, 0,                         /* 22 movss xmm11, [rip+the ramp] (rel32 at 27) */
    0xE9, 0, 0, 0, 0,                                                 /* 31 jmp 0xF65203 (rel32 at 32) */
    0, 0, 0, 0, 0, 0, 0, 0};                                          /* 36 1/64, 40 the larger map's 1/ramp */
/* The area heights' two stores: the call of the rim (0xF64A60) and movss [rbx+0x88], xmm0. */
static const struct { uint32_t site; uint8_t pin[13]; } kTerrainStores[2] = {
    {0xF655A4u, {0xE8, 0xB7, 0xF4, 0xFF, 0xFF, 0xF3, 0x0F, 0x11, 0x83, 0x88, 0x00, 0x00, 0x00}},
    {0xF656F4u, {0xE8, 0x67, 0xF3, 0xFF, 0xFF, 0xF3, 0x0F, 0x11, 0x83, 0x88, 0x00, 0x00, 0x00}}};
/* At the store a call has just returned, so the volatile registers are free but xmm0 (the height); rbx is the area
 * (position +0x80, height +0x88), rsp 16-aligned. r13 is not the pass context here (0xF654E2 reuses it until 0xF65731):
 * the context is the frame's [rbp-0x40] (0xF64E91), [ctx+8] = lg+0x2D20, [ctx+0x18] = lg. */
static const uint8_t kTerrainStoreStub[96] = {
    0x48, 0x8B, 0x45, 0xC0,                                           /*  0 mov rax, [rbp-0x40] (the context) */
    0x48, 0x8B, 0x40, 0x08,                                           /*  4 mov rax, [rax+8] (lg+0x2D20) */
    0x81, 0x78, 0x5C, 0, 0, 0, 0,                                     /*  8 cmp dword [rax+0x5c], the larger disk (at 11) */
    0x75, 0x3B,                                                       /* 15 jne 76 */
    0x0F, 0x2F, 0x05, 0, 0, 0, 0,                                     /* 17 comiss xmm0, [rip+0.0] (rel32 at 20) */
    0x73, 0x32,                                                       /* 24 jae 76 */
    0x48, 0x83, 0xEC, 0x30,                                           /* 26 sub rsp, 0x30 */
    0xF3, 0x0F, 0x11, 0x44, 0x24, 0x20,                               /* 30 movss [rsp+0x20], xmm0 */
    0x48, 0x8B, 0x4D, 0xC0,                                           /* 36 mov rcx, [rbp-0x40] (the context) */
    0x48, 0x8B, 0x49, 0x18,                                           /* 40 mov rcx, [rcx+0x18] (lg) */
    0x48, 0x8B, 0xD3,                                                 /* 44 mov rdx, rbx (the area) */
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /* 47 mov rax, TerrainNearObjective (imm64 at 49) */
    0xFF, 0xD0,                                                       /* 57 call rax */
    0xF3, 0x0F, 0x10, 0x44, 0x24, 0x20,                               /* 59 movss xmm0, [rsp+0x20] */
    0x48, 0x83, 0xC4, 0x30,                                           /* 65 add rsp, 0x30 */
    0x85, 0xC0,                                                       /* 69 test eax, eax */
    0x74, 0x03,                                                       /* 71 je 76 */
    0x0F, 0x57, 0xC0,                                                 /* 73 xorps xmm0, xmm0 (raised to 0) */
    0xF3, 0x0F, 0x11, 0x83, 0x88, 0x00, 0x00, 0x00,                   /* 76 movss [rbx+0x88], xmm0 */
    0xE9, 0, 0, 0, 0,                                                 /* 84 jmp back (rel32 at 85) */
    0, 0, 0,                                                          /* 89 */
    0, 0, 0, 0};                                                      /* 92 0.0 */
#define DIF_OBJECTIVE_LOCATIONS 0x8996B0u /* G+: the important locations, 0x108 each, where the game has them */
static uint32_t ImportantLocations(uint32_t* places); /* difficulty_block.c: where they are now, and their places */
#define DIF_OBJECTIVE_COUNT 0x11A715Cu    /* G+: their count */
#define DIF_OBJECTIVE_MARGIN 16.0f        /* m past the stamp's radius */
#define DIF_LOCATION_GROUP_SET 0xADC32FAAu
static volatile LONG g_terrainRaised, g_terrainChecked; /* heights raised, and below 0 m checked */
static volatile LONG g_terrainObjectives, g_terrainSized;  /* the last check's objectives, and those with a radius */

static int TerrainRampWanted(void)
{
    wchar_t value[8];
    DWORD length = GetEnvironmentVariableW(L"HELLFORGE_D11_TERRAIN_RAMP", value, 8);
    return !(length == 1 && value[0] == L'0');
}

static int DryObjectivesWanted(void)
{
    wchar_t value[8];
    DWORD length = GetEnvironmentVariableW(L"HELLFORGE_D11_DRY_OBJECTIVES", value, 8);
    return !(length == 1 && value[0] == L'0');
}

/* A location's stamp radius (its item's +0xD8), or 0 when it has none: the record's +0 is its set (a location-group set
 * when +8 is DIF_LOCATION_GROUP_SET: group +0xA1, item +0xA2; else +0 points at the item). */
static float ObjectiveRadius(const uint8_t* location)
{
    uint64_t set;
    uint32_t kind;
    uint8_t group = location[0xA1], item = location[0xA2];
    memcpy(&set, location, 8);
    memcpy(&kind, location + 8, 4);
    if (item == 0xFF || set == 0) return 0.0f;
    const uint8_t* record = NULL;
    if (kind == DIF_LOCATION_GROUP_SET)
    {
        uint64_t groups;
        uint32_t groupCount, itemCount;
        memcpy(&groups, (const uint8_t*)(uintptr_t)set + 0x28, 8);
        memcpy(&groupCount, (const uint8_t*)(uintptr_t)set + 0x30, 4);
        if (group == 0xFF || groups == 0 || group >= groupCount) return 0.0f;
        const uint8_t* entry = (const uint8_t*)(uintptr_t)groups + 0x18 * (size_t)group;
        uint64_t items;
        memcpy(&items, entry + 8, 8);
        memcpy(&itemCount, entry + 0x10, 4);
        if (items == 0 || item >= itemCount) return 0.0f;
        record = (const uint8_t*)(uintptr_t)items + 0x1C8 * (size_t)item;
    }
    else
    {
        uint64_t direct;
        memcpy(&direct, (const uint8_t*)(uintptr_t)set, 8);
        record = (const uint8_t*)(uintptr_t)direct;
    }
    float radius = 0.0f;
    if (record != NULL) memcpy(&radius, record + 0xD8, 4);
    return radius > 0.0f && radius < 1024.0f ? radius : 0.0f;
}

/* Called by the store stubs for a height below 0 on a larger map: 1 if the area lies within an objective's stamp radius
 * + DIF_OBJECTIVE_MARGIN (it is then stored as 0). */
static int TerrainNearObjective(uintptr_t level, const uint8_t* area)
{
    uint64_t block = 0;
    if (level == 0) return 0;
    memcpy(&block, (const void*)(level + 0x2C8), 8);
    if (block == 0) return 0;
    uint32_t count;
    float x, y;
    memcpy(&count, (const void*)(uintptr_t)(block + DIF_OBJECTIVE_COUNT), 4);
    memcpy(&x, area + 0x80, 4);
    memcpy(&y, area + 0x84, 4);
    uint32_t places, list = ImportantLocations(&places);
    if (count > places) count = places;
    InterlockedIncrement(&g_terrainChecked);
    LONG sized = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint8_t* location = (const uint8_t*)(uintptr_t)(block + list + 0x108 * (uint64_t)i);
        float lx, ly;
        memcpy(&lx, location + 0xA4, 4);
        memcpy(&ly, location + 0xA8, 4);
        float reach = ObjectiveRadius(location);
        if (reach <= 0.0f) continue;
        sized++;
        reach += DIF_OBJECTIVE_MARGIN;
        float dx = x - lx, dy = y - ly;
        if (dx * dx + dy * dy <= reach * reach)
        {
            InterlockedIncrement(&g_terrainRaised);
            InterlockedExchange(&g_terrainObjectives, (LONG)count);
            return 1;
        }
    }
    InterlockedExchange(&g_terrainObjectives, (LONG)count);
    InterlockedExchange(&g_terrainSized, sized);
    return 0;
}

static const char* TerrainPinsFailed(uintptr_t game)
{
    if (!LargerMaps()) return NULL;
    uint8_t bytes[13];
    if (SafeRead(game + DIF_TERRAIN_RAMP_SITE, bytes, 9) || memcmp(bytes, kTerrainRampPin, 9) != 0)
        return "the area heights' route ramp is not the reviewed one (a map_scale above 1 needs it)";
    for (int i = 0; i < 2; i++)
        if (SafeRead(game + kTerrainStores[i].site, bytes, 13) || memcmp(bytes, kTerrainStores[i].pin, 13) != 0)
            return "the area heights' stores are not the reviewed ones (a map_scale above 1 needs them)";
    return NULL;
}

static uint8_t* TerrainRel32(uint8_t* p, uintptr_t target) { int32_t rel = (int32_t)(int64_t)(target - (uintptr_t)(p + 4)); memcpy(p, &rel, 4); return p + 4; }

/* The stubs on a page of their own, then each site sent to its stub. */
static const char* InstallTerrain(DifficultyInstall* d)
{
    if (!LargerMaps()) return NULL;
    int ramp = TerrainRampWanted(), dry = DryObjectivesWanted();
    if (!ramp && !dry) return NULL;
    uintptr_t game = d->game;
    uint8_t* code = AllocateNearGame(game, 0x1000);
    if (code == NULL) return "no memory within 2 GB of game.dll for the terrain heights";
    float diskBits = d->disk / 1024.0f;
    float spacing = d->spacingValue != NULL && *d->spacingValue > 0.0f ? *d->spacingValue : 40.0f;
    float ramps[2] = {1.0f / 64.0f, 1.0f / (64.0f * spacing / 40.0f)};
    uint8_t* rampStub = code;
    memcpy(rampStub, kTerrainRampStub, sizeof kTerrainRampStub);
    memcpy(rampStub + 36, ramps, 8);
    TerrainRel32(rampStub + 5, (uintptr_t)(rampStub + 36));
    memcpy(rampStub + 16, &diskBits, 4);
    TerrainRel32(rampStub + 27, (uintptr_t)(rampStub + 40));
    TerrainRel32(rampStub + 32, game + DIF_TERRAIN_RAMP_BACK);
    uint8_t* storeStubs[2];
    uint64_t check = (uint64_t)(uintptr_t)TerrainNearObjective;
    for (int i = 0; i < 2; i++)
    {
        uint8_t* p = storeStubs[i] = code + 0x40 + 0x60 * i;
        memcpy(p, kTerrainStoreStub, sizeof kTerrainStoreStub);
        memcpy(p + 11, &diskBits, 4);
        TerrainRel32(p + 20, (uintptr_t)(p + 92));
        memcpy(p + 49, &check, 8);
        TerrainRel32(p + 85, game + kTerrainStores[i].site + 13);
    }
    DWORD protect;
    if (!VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ, &protect)) return "the terrain heights' stubs could not be made executable";
    FlushInstructionCache(GetCurrentProcess(), code, 0x1000);
    if (ramp)
    {
        uint8_t jump[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
        int32_t rel = (int32_t)(int64_t)((uintptr_t)rampStub - (game + DIF_TERRAIN_RAMP_SITE + 5));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + DIF_TERRAIN_RAMP_SITE, jump, 9)) return "the area heights' route ramp could not be patched";
    }
    for (int i = 0; i < 2 && dry; i++) /* the store only: the call before it stays */
    {
        uint8_t jump[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
        int32_t rel = (int32_t)(int64_t)((uintptr_t)storeStubs[i] - (game + kTerrainStores[i].site + 5 + 5));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + kTerrainStores[i].site + 5, jump, 8)) return "the area heights' stores could not be patched";
    }
    Note("difficulty: terrain on larger maps: route ramp %s (64 m x %.3f), objectives' ground %s", ramp ? "scaled" : "level 10's",
        (double)(spacing / 40.0f), dry ? "kept at or above 0 m" : "as generated");
    return NULL;
}

/* DifficultyWatch: the heights below 0 m checked near objectives and those raised, once they stop changing (one
 * generation's worth), with the objectives the last check saw and how many had a stamp radius. */
static void WatchTerrain(void)
{
    static LONG seen, logged, raised;
    LONG now = g_terrainChecked;
    int moving = now != seen;
    seen = now;
    if (moving || now == logged) return;
    LONG up = g_terrainRaised;
    Note("difficulty: terrain: %ld area height(s) below 0 m checked against %ld objectives (%ld with a stamp radius); %ld raised to 0 "
        "within their stamps", now - logged, (long)g_terrainObjectives, (long)g_terrainSized, up - raised);
    logged = now;
    raised = up;
}
