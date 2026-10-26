/* Level 11's larger maps (difficulty.c, a map_scale above 1): the generator's radii and spacing, the route cells, the
 * height zones, the city generator's capacities, the preview, the stamp index and the navigation grid; and the physics
 * pools. Included by hellforge_modloader.c after difficulty_cities.c. */

/* Larger maps. The route-cell generator (0xFD03E0) fills fixed arrays without checking them: vertices 975 (850 at
 * level 10), edges 1621, cells 1024. Its sites are a Poisson disc of the generation settings' spacing (+4, 40 m) in a
 * disk of +0xC (680 m) around the world's centre (the world is 2048 m), so a larger playable radius alone overflows
 * them (1.25 x crashed at 0xF6D9A0, 1.6 x at 0xC182E7). A level above 1 x gets, per mission, a site disk and fade
 * radius that cover its border and a spacing that keeps level 10's number of sites: level init (0xF71220) copies the
 * settings' radii into the level (lg+0x2D78..+0x2D8C) and the two spacing reads follow the level's radius.
 * GenerateVoronoi (op 0x25, 0xF3F8E0) lays a 16 m grid (settings +0) over the same square, keeps the points within the
 * disk and runs jc_voronoi on them: level 10 makes 5672 sites, 5839 vertices and ~11430 edges against arrays of 5680,
 * 5849 and 11528, unchecked. Its spacing grows with the disk, rounded up to a quarter metre: grid points exact in
 * float, as level 10's are. An inexact spacing (24.06 m) gives near-degenerate circles and 7275 vertices; the overrun
 * array then fails the pass's own vertex check (0xF4003F) and the pass retries forever. 2 x: 24.5 m, 5468 sites,
 * 5635 vertices, ~11014 edges (game.dll's jc_voronoi run offline). The structured
 * maps (map type 2: mega cities, factories) grow too: their playable
 * radius is the distance table's (0x2143170, per level and sub-kind), scaled for level 11; their sites are a fixed
 * template stretched over the rect (0xFB9610), and revoronoi (op 0xA3, 0xFD0E00) refills the land around the city with a
 * Poisson disc at the nature cell size, which the revoronoi stubs grow like the route cells' (kReVoronoiStubs). */
#define DIF_MAP_TYPE_SITE 0xF71CD8u    /* mov [r13 + 0x2de0], rdx: the map type, after the radii (r13 = lg) */
#define DIF_SPACING_SITE 0xF3614Du     /* movss xmm2, [rax + 4]: the sites' spacing (r14 = pass, [r14 + 0x18] = lg) */
#define DIF_SPACING_SITE_2 0xF3732Cu   /* subss xmm6, [rax + 4]: a cell's distance less the spacing (lg = [[rsp + 0x60] + 0x18]) */
/* GenerateRouteCellVoronoi (op 3, 0xF93800) fills each route cell with points at its cell type's size (+0x18C) / 2
 * (grid) or / 4 (Poisson) and builds their diagrams: level 10 makes 11954 areas of 13224 (G+0xEE10C8) and 23673
 * vertices of 26205, unchecked, in proportion to the generated area. An overrun crashed GenerateSubregionHeights
 * (0xF64878). The spacing grows with the disk (x 1.04 for room), rounded up to a quarter metre. */
#define DIF_CELL_SPACING_SITE 0xF9420Fu /* divss xmm2, xmm0; mov rdx, [rdx] (rdx = pass, [rdx + 0x18] = lg) */
/* FinalizeVoronoi (op 0xA4, 0xF68050), on map type 1, rebuilds the route cells from points laid over the whole site
 * square at each cell type's size (+0x18C), grid or Poisson: 2.25 x the points of level 10 overran the 1024 route
 * cells (crash in its edge sort, 0xFCF838, at cell 1024). Same scaling as the route cells' areas. */
#define DIF_FINALIZE_SPACING_SITE 0xF68273u /* movss xmm2, [r13 + 0x18c] ([rsp + 0x70] = pass, [pass + 0x18] = lg) */
#define DIF_GRID_SITE 0xF3FB04u        /* mov [rsp+0x20], r14; movss xmm2, [rax]: GenerateVoronoi's grid (rsi = pass, [rsi + 0x18] = lg) */
static const uint8_t kMapTypePin[7] = {0x49, 0x89, 0x95, 0xE0, 0x2D, 0x00, 0x00};
static const uint8_t kSpacingPin[5] = {0xF3, 0x0F, 0x10, 0x50, 0x04};
static const uint8_t kSpacingPin2[5] = {0xF3, 0x0F, 0x5C, 0x70, 0x04};
/* The generator's stamp index (0xC04B80: placed stamps, locations and roads by area, for overlap tests) has 10757
 * slots (0x2A05) after a bitset; level 10 uses 4892, and the 17 inserters (0xC04EF0) write through a full index's null
 * (PlaceStampWithWeightRule crashed at 0xF3516C). Larger maps get 16383 slots: the same 14 index bits, so the key
 * layout is unchanged. The size (0xC048E0), the layout check (0xC04AB0) and the index (0xC04B80) agree. Every index is
 * sized at creation; levels are generated well after the loader installs this. */
static const struct { uint32_t rva; uint8_t length; uint8_t pin[7], bytes[7]; } kStampIndex[5] = {
    {0xC0498Bu, 5, {0x05, 0x1F, 0xA8, 0x00, 0x00}, {0x05, 0x07, 0x00, 0x01, 0x00}},                     /* add eax, slots x 4 + 7 */
    {0xC04AEAu, 5, {0xB8, 0x05, 0x2A, 0x00, 0x00}, {0xB8, 0xFF, 0x3F, 0x00, 0x00}},                     /* mov eax, slots */
    {0xC04C1Au, 6, {0x41, 0xBD, 0x05, 0x2A, 0x00, 0x00}, {0x41, 0xBD, 0xFF, 0x3F, 0x00, 0x00}},         /* mov r13d, slots */
    {0xC04CB2u, 7, {0x48, 0x8D, 0x81, 0x18, 0xA8, 0x00, 0x00}, {0x48, 0x8D, 0x81, 0x00, 0x00, 0x01, 0x00}}, /* lea rax, [rcx + (slots + 1) x 4] */
    {0xC04CCCu, 7, {0xC7, 0x47, 0x1C, 0x06, 0x2A, 0x00, 0x00}, {0xC7, 0x47, 0x1C, 0x00, 0x40, 0x00, 0x00}}}; /* mov [rdi+0x1c], slots + 1 */
static const uint8_t kFinalizeSpacingPin[9] = {0xF3, 0x41, 0x0F, 0x10, 0x95, 0x8C, 0x01, 0x00, 0x00};
static const uint8_t kFinalizeSpacingStub[57] = {
    0xF3, 0x41, 0x0F, 0x10, 0x95, 0x8C, 0x01, 0x00, 0x00,             /*  0 movss xmm2, [r13+0x18c] */
    0x48, 0x8B, 0x44, 0x24, 0x70,                                     /*  9 mov rax, [rsp+0x70] */
    0x48, 0x8B, 0x40, 0x18,                                           /* 14 mov rax, [rax+0x18] */
    0x81, 0xB8, 0x7C, 0x2D, 0x00, 0x00, 0, 0, 0, 0,                   /* 18 cmp dword [rax+0x2d7c], disk / 1024 (at 24) */
    0x75, 0x16,                                                       /* 28 jne 52 */
    0xF3, 0x0F, 0x59, 0x15, 0, 0, 0, 0,                               /* 30 mulss xmm2, [rip+factor x 4] (rel32 at 34) */
    0x66, 0x0F, 0x3A, 0x0A, 0xD2, 0x02,                               /* 38 roundss xmm2, xmm2, ceil */
    0xF3, 0x0F, 0x59, 0x15, 0, 0, 0, 0,                               /* 44 mulss xmm2, [rip+0.25] (rel32 at 48) */
    0xE9, 0, 0, 0, 0};                                                /* 52 jmp back (rel32 at 53) */
/* The navigation world (research-beacons): the engine sets it up once at startup (exe 0x7B45C0) as 8 x 8 tiles of 128 m,
 * +-512 m around the centre, and every mission reuses it. Hellpods and mission beacons stick only where the navmesh is
 * (0x6A0D6D), and enemies walk only there, so a larger map's far side (up to 778 m out) had none: blue stratagems would
 * not stick. The engine's block for it is sized for 8 x 8 tiles, so the grid keeps them and their size changes: NavRetile
 * writes 256 m tiles (the whole 2048 m world) before a larger map's navmesh grid is set up (setup_navmeshgrid, pass 0x68,
 * handler 0xC17DFC: the nav world id in ecx, the pass context in r15, its level at +0x18) and the vanilla 128 m before
 * any other level's and before the ship's (0xADA075). Fields of the nav world W = [[exe+0x27C30B8] + 8 id] (id < [exe+
 * 0x27C30C0]): +0x08/+0x0C min x, y, +0x14/+0x18 max x, y, +0x6710/+0x6712 the grid (u16, 8 x 8), +0x6718 the tile,
 * +0x671C..+0x6728 the grid's min and max; the engine reads them afresh. */
#define DIF_NAV_SITE 0xC17E2Eu         /* mov rax, [0x33263C8]; call [rax+8]; jmp 0xC19E1C (15 bytes) */
#define DIF_NAV_BACK 0xC19E1Cu
#define DIF_NAV_SHIP_SITE 0xADA075u    /* mov ecx, [r14+0x10f0]; call [rax+8] (10 bytes) */
#define DIF_NAV_SHIP_BACK 0xADA07Fu
#define DIF_NAV_SYSTEM 0x33263C8u
#define DIF_NAV_WORLDS 0x27C30B8u      /* exe: the address of the nav world pointers */
#define DIF_NAV_COUNT 0x27C30C0u       /* exe: their count */
#define DIF_NAV_PIN 0x7B73F0u          /* exe: a reader of both, checked */
static const uint8_t kNavPin[15] = {0x48, 0x8B, 0x05, 0x93, 0xE5, 0x70, 0x02, 0xFF, 0x50, 0x08, 0xE9, 0xDF, 0x1F, 0x00, 0x00};
static const uint8_t kNavShipPin[10] = {0x41, 0x8B, 0x8E, 0xF0, 0x10, 0x00, 0x00, 0xFF, 0x50, 0x08};
static const uint8_t kNavExePin[23] = {0x48, 0x83, 0xEC, 0x28, 0x39, 0x0D, 0xC6, 0xBC, 0x00, 0x02, 0x0F, 0x86, 0xE7, 0x00, 0x00, 0x00,
    0x48, 0x8B, 0x05, 0xB1, 0xBC, 0x00, 0x02};
static const uint8_t kNavStub[145] = {
    STUB_PUSH_VOLATILE,                                               /*   0 push rax rcx rdx r8 r9 r10 r11 */
    0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00,                         /*  11 sub rsp, 0x88 (aligned: 7 pushes + 0x88) */
    STUB_SAVE_XMM,                                                    /*  18 movdqu [rsp+0x20..0x70], xmm0..xmm5 */
    0x49, 0x8B, 0x57, 0x18,                                           /*  54 mov rdx, [r15+0x18] (the level; ecx = the id) */
    STUB_CALL_IMM64,                                                  /*  58 mov rax, NavRetile (imm64 at 60); call rax */
    STUB_LOAD_XMM,                                                    /*  70 movdqu xmm0..xmm5, [rsp+0x20..0x70] */
    0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00,                         /* 106 add rsp, 0x88 */
    STUB_POP_VOLATILE,                                                /* 113 pop r11 r10 r9 r8 rdx rcx rax */
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /* 124 mov rax, game+0x33263C8 (imm64 at 126) */
    0x48, 0x8B, 0x00,                                                 /* 134 mov rax, [rax] */
    0xFF, 0x50, 0x08,                                                 /* 137 call [rax+8] */
    0xE9, 0, 0, 0, 0};                                                /* 140 jmp 0xC19E1C (rel32 at 141) */
static const uint8_t kNavShipStub[144] = {
    STUB_PUSH_VOLATILE,                                               /*   0 push rax rcx rdx r8 r9 r10 r11 */
    0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00,                         /*  11 sub rsp, 0x88 (aligned: 7 pushes + 0x88) */
    STUB_SAVE_XMM,                                                    /*  18 movdqu [rsp+0x20..0x70], xmm0..xmm5 */
    0x41, 0x8B, 0x8E, 0xF0, 0x10, 0x00, 0x00,                         /*  54 mov ecx, [r14+0x10f0] (the id) */
    0x31, 0xD2,                                                       /*  61 xor edx, edx (no level: vanilla) */
    STUB_CALL_IMM64,                                                  /*  63 mov rax, NavRetile (imm64 at 65); call rax */
    STUB_LOAD_XMM,                                                    /*  75 movdqu xmm0..xmm5, [rsp+0x20..0x70] */
    0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00,                         /* 111 add rsp, 0x88 */
    STUB_POP_VOLATILE,                                                /* 118 pop r11 r10 r9 r8 rdx rcx rax */
    0x41, 0x8B, 0x8E, 0xF0, 0x10, 0x00, 0x00,                         /* 129 mov ecx, [r14+0x10f0] */
    0xFF, 0x50, 0x08,                                                 /* 136 call [rax+8] */
    0xE9, 0, 0, 0, 0};                                                /* 139 jmp 0xADA07F (rel32 at 140) */
/* The tiles' terrain build (exe 0x89A6C0, Recast: research-navtiles) sizes its heightfield from a hard-coded 128 m
 * (0x89A85E: movss xmm0, [rip+128.0], cells = 128 / 0.5) while it rasterises the tile's real bounds: a 256 m tile wrote
 * past its rows (a level-11 load crashed in addSpan, exe+0x8BCE66). Its displacement is pointed at the exe's own 256.0
 * (0x16AA7C0) for 256 m tiles and back at 128.0 (0x16A9DB0, also read elsewhere: not written) for 128 m ones. */
#define DIF_NAV_TILE_SIZE_READ 0x89A85Eu
static const uint8_t kNavTileSizeRead[8] = {0xF3, 0x0F, 0x10, 0x05, 0x4A, 0xF5, 0xE0, 0x00};
static const uint8_t kNavTileSize256[4] = {0x5A, 0xFF, 0xE0, 0x00};
static int g_navTiles256; /* the exe's terrain build and its 256.0 are as reviewed */
static uintptr_t g_navExe;
static volatile LONG g_navLarger, g_navVanilla;
/* Before a navmesh grid is set up: 256 m tiles for a larger map (its generated disk, lg+0x2D8C, past vanilla's 681 m),
 * else 128 m. Only an 8 x 8 grid is touched. */
static void NavRetile(uint32_t id, uintptr_t level)
{
    uintptr_t exe = g_navExe, worlds = 0, world = 0;
    uint32_t count = 0;
    uint16_t grid[2] = {0, 0};
    float disk = 0.0f;
    if (exe == 0 || SafeRead(exe + DIF_NAV_COUNT, &count, 4) || id >= count || SafeRead(exe + DIF_NAV_WORLDS, &worlds, 8) || worlds == 0 ||
        SafeRead(worlds + 8 * (uintptr_t)id, &world, 8) ||
        world == 0 || SafeRead(world + 0x6710, grid, 4) || grid[0] != 8 || grid[1] != 8) return;
    int larger = g_navTiles256 && level != 0 && !SafeRead(level + 0x2D8C, &disk, 4) && disk > 682.0f;
    if (g_navTiles256 && ApiWrite(exe + DIF_NAV_TILE_SIZE_READ + 4, larger ? kNavTileSize256 : kNavTileSizeRead + 4, 4)) return;
    float lo = larger ? -1023.75f : -511.75f, hi = larger ? 1024.25f : 512.25f;
    float min[2] = {lo, lo}, max[2] = {hi, hi}, tiles[5] = {larger ? 256.0f : 128.0f, lo, lo, hi, hi};
    if (ApiWrite(world + 0x08, min, sizeof min) || ApiWrite(world + 0x14, max, sizeof max) || ApiWrite(world + 0x6718, tiles, sizeof tiles)) return;
    InterlockedIncrement(larger ? &g_navLarger : &g_navVanilla);
}

/* The physics pools: game.dll's plugin setup (0x4EE160, once per process,
 * about 5 s after the loader starts) fills the engine's physics config with fixed sizes, and the exe (0x795630) builds
 * every world's pools from it in one block. Every enemy has a ragdoll (corpses keep theirs) of 25..34 actors, and the
 * ragdoll actor store (8192 per world) filled at about 230 enemies: its allocator then returns -1, which the ragdoll
 * builder stores as a handle unchecked (exe 0x7A307F) and the game ends. Pool index widths follow their sizes (the static
 * pool already has 131072), so the sizes are raised before the setup runs: ragdoll actors 8192 -> 32768, ragdolls
 * 512 -> 1024, and the Havok world's motions 36864 -> 73728 (about one per non-static actor). A larger mega city
 * used up the world's 131072 bodies while spawning: the allocator's "none"
 * (0xFFFFFF, 0xD4588C) became a building's body and its navmesh cutter read it (exe 0xCB304C). Bodies and static actors
 * get 262144 (body ids are 24-bit; 0x814F00 keeps flags in bits 21..23, so below 2^21), navmesh cutters 65536. The
 * Game World's render objects ("max render objects", 0xAB189D in the same
 * world creation) size its culling systems (N/8 for the main visibility set, 98304, which then ran out at exe
 * 0x58E14B): 0xC0000 -> 0x200000 (handles keep 24 bits, 2 generation bits left; ~+120 MB). Dynamic actors 32768 ->
 * 131072, keyframed 8192 -> 32768 and motions 73728 -> 131072 follow the same 4 x units.
 * [game+0x3326380] is set by the setup's first call (0x4EDC62): while it is 0, the setup has not started. */
static const struct { uint32_t rva; uint8_t length, at; uint8_t pin[10]; uint32_t value; } kPhysicsPools[9] = {
    {0x4EE32Bu, 7, 3, {0xC7, 0x45, 0x9C, 0x00, 0x20, 0x00, 0x00}, 0x8000u},                        /* config +0x2C ragdoll actors */
    {0x4EE35Cu, 7, 3, {0xC7, 0x45, 0xAC, 0x00, 0x02, 0x00, 0x00}, 0x400u},                         /* config +0x3C ragdolls */
    {0xAB19F8u, 10, 6, {0xC7, 0x85, 0x20, 0x05, 0x00, 0x00, 0x00, 0x90, 0x00, 0x00}, 0x20000u},   /* Havok world motions */
    {0xAB19EEu, 10, 6, {0xC7, 0x85, 0x1C, 0x05, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00}, 0x40000u},   /* Havok world bodies */
    {0x4EE324u, 7, 3, {0xC7, 0x45, 0x98, 0x00, 0x00, 0x02, 0x00}, 0x40000u},                       /* config +0x28 static actors */
    {0x4EE371u, 7, 3, {0xC7, 0x45, 0xBC, 0x00, 0x80, 0x00, 0x00}, 0x10000u},                       /* config +0x4C navmesh cutters */
    {0x4EE31Du, 7, 3, {0xC7, 0x45, 0x94, 0x00, 0x80, 0x00, 0x00}, 0x20000u},                       /* config +0x24 dynamic actors */
    {0x4EE332u, 7, 3, {0xC7, 0x45, 0x90, 0x00, 0x20, 0x00, 0x00}, 0x8000u},                        /* config +0x20 keyframed actors */
    {0xAB189Du, 8, 4, {0xC7, 0x44, 0x24, 0x58, 0x00, 0x00, 0x0C, 0x00}, 0x200000u}};             /* Game World render objects */
#define DIF_PHYSICS_SETUP 0x3326380u
static int g_physicsPools; /* 0 waiting, 1 enlarged, -1 left as they are */
/* Polled from the loader's thread once game.dll is decoded. */
static void PhysicsPools(uintptr_t game)
{
    uint64_t setup = 1;
    if (g_physicsPools != 0 || game == 0 || SafeRead(game + DIF_PHYSICS_SETUP, &setup, 8)) return;
    if (setup != 0)
    {
        g_physicsPools = -1;
        Note("physics: the engine's pools were already built: ragdolls keep 8192 actors (about 230 enemies at once)");
        return;
    }
    for (int i = 0; i < 9; i++)
    {
        uint8_t bytes[10];
        if (SafeRead(game + kPhysicsPools[i].rva, bytes, kPhysicsPools[i].length) || memcmp(bytes, kPhysicsPools[i].pin, kPhysicsPools[i].length) != 0)
        {
            g_physicsPools = -1;
            Note("physics: the physics config is not the reviewed one: its pools keep their sizes");
            return;
        }
    }
    for (int i = 0; i < 9; i++)
        if (ApiWrite(game + kPhysicsPools[i].rva + kPhysicsPools[i].at, &kPhysicsPools[i].value, 4))
        {
            g_physicsPools = -1;
            Note("physics: a physics pool could not be enlarged");
            return;
        }
    g_physicsPools = 1;
    Note("physics: pools enlarged before the engine built them: ragdoll actors 32768, ragdolls 1024, motions 131072, bodies and static actors 262144, dynamic actors 131072, keyframed 32768, navmesh cutters 65536 per world; render objects 2097152");
}
/* SpreadLocations (op 0x8B, 0xF7C310) picks the nearest location of a kind as a reference (r12d, -1 when none
 * qualifies) and sorts its candidates by their distance from it (0xF7A430) without checking: a level-11 city map (normal
 * size, Hell Forge's extra outposts and objectives) found none, and the sort read location -1 (a crash on the preview).
 * When none was found, location 0 is the reference; every other case is unchanged. */
#define DIF_SPREAD_REFERENCE 0xF7CEC8u   /* mov eax, r12d; imul r12, rax, 0xe0 */
#define DIF_SPREAD_REFERENCE_BACK 0xF7CED2u
static const uint8_t kSpreadReferencePin[10] = {0x41, 0x8B, 0xC4, 0x4C, 0x69, 0xE0, 0xE0, 0x00, 0x00, 0x00};
static const uint8_t kSpreadReferenceStub[24] = {
    0x41, 0x83, 0xFC, 0xFF, 0x75, 0x03,                               /*  0 cmp r12d, -1; jne 9 */
    0x45, 0x31, 0xE4,                                                 /*  6 xor r12d, r12d (none: location 0) */
    0x41, 0x8B, 0xC4, 0x4C, 0x69, 0xE0, 0xE0, 0x00, 0x00, 0x00,       /*  9 mov eax, r12d; imul r12, rax, 0xe0 */
    0xE9, 0, 0, 0, 0};                                                /* 19 jmp 0xF7CED2 (rel32 at 20) */
/* The game's hash insert (0x172F790, 704 callers) probes every slot for the key or an empty one and, finding neither in
 * a full table, writes the entry through a null slot (0x172F7F2, writing 0x4). A level-11 city map (twice the radius)
 * registered more destructibles than the world's destruction-record map holds ([game + 0x346D528] + 0xC5F48, 0x8000
 * slots set at 0x13DF877; its records at +0xC5F78) and ended the game as the mission launched. A full table now skips
 * the entry and counts it (DifficultyWatch logs the count): that destructible has no record lookup instead of the game
 * ending. A table with room is unchanged. The map cannot simply grow: its storage is inline, followed by another table. */
#define DIF_HASH_INSERT_END 0x172F7F2u
static const uint8_t kHashInsertEndPin[12] = {0x89, 0x71, 0x04, 0x48, 0x8B, 0x74, 0x24, 0x10, 0x44, 0x89, 0x09, 0xC3};
static const uint8_t kHashInsertEndStub[26] = {
    0x48, 0x85, 0xC9, 0x75, 0x09,                                     /*  0 test rcx, rcx; jne 14 */
    0xF0, 0xFF, 0x05, 0xF4, 0x0F, 0x00, 0x00,                         /*  5 lock inc dword [rip+0xff4] (the count, next page) */
    0xEB, 0x06,                                                       /* 12 jmp 20 */
    0x89, 0x71, 0x04, 0x44, 0x89, 0x09,                               /* 14 mov [rcx+4], esi; mov [rcx], r9d */
    0x48, 0x8B, 0x74, 0x24, 0x10, 0xC3};                              /* 20 mov rsi, [rsp+0x10]; ret */
static volatile LONG* g_hashFullCount;
/* The height zones (force_flatten_zones, 0xF4C100, pass op 0x3E): each group of connected cells whose type sets a height
 * (+0x208) gets an entry of a 16-entry table on the stack (rbp+0x60, 12 bytes each, unchecked), and the second loop
 * flattens every zone cell to its group's height (0xF4C3E6). Level 10 stays below 16; a 2 x map made more and the stack
 * cookie check ended the game (0xC0000409). The table moves to the generation arena, sized by the route cells (r12d;
 * groups can never outnumber them): the clear at 0xF4C1CD becomes the
 * allocation (its pointer kept at rbp+0x60), the loop's lea of the table (0xF4C29C) a load of that pointer, and the
 * height read (0xF4C3E6) goes through it. (Capping the groups at 16 left later cells with group 0's height: the outer
 * cities sank under a flat plain, roads and buildings buried.) */
#define DIF_ZONE_SITE 0xF4C260u
static const uint8_t kZonePin[14] = {0x41, 0x80, 0x3C, 0x24, 0x00, 0x45, 0x8B, 0xC6, 0x0F, 0x85, 0xC7, 0x00, 0x00, 0x00};
#define DIF_ZONE_CLEAR 0xF4C1CDu       /* xor edx,edx; mov [rbp-0x48],rax; mov r8d,0xc0; lea rcx,[rbp+0x60]; call memset */
#define DIF_ZONE_CLEAR_BACK 0xF4C1E2u
#define DIF_ZONE_TABLE_LEA 0xF4C29Cu   /* lea rbx, [rbp+0x60] (byte +1: 8D -> 8B, a load) */
#define DIF_ZONE_HEIGHT 0xF4C3E6u      /* movss xmm9, [rbp+rcx*4+0x68] */
#define DIF_ZONE_HEIGHT_BACK 0xF4C3EDu
static const uint8_t kZoneClearPin[21] = {0x33, 0xD2, 0x48, 0x89, 0x45, 0xB8, 0x41, 0xB8, 0xC0, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x4D, 0x60,
    0xE8, 0xBE, 0xE8, 0x13, 0x01};
static const uint8_t kZoneTableLeaPin[8] = {0x48, 0x8D, 0x5D, 0x60, 0x48, 0x8D, 0x1C, 0x8B};
static const uint8_t kZoneHeightPin[7] = {0xF3, 0x44, 0x0F, 0x10, 0x4C, 0x8D, 0x68};
static const uint8_t kZoneClearStub[62] = {
    0x48, 0x89, 0x45, 0xB8,                                           /*  0 mov [rbp-0x48], rax (the replaced store) */
    0x44, 0x89, 0xE2, 0xFF, 0xC2, 0x8D, 0x14, 0x52, 0xC1, 0xE2, 0x02, /*  4 edx = (route cells + 1) x 12 */
    0x48, 0x8B, 0x0E,                                                 /* 15 mov rcx, [rsi] (the generation arena) */
    0x41, 0xB8, 0x08, 0x00, 0x00, 0x00,                               /* 18 mov r8d, 8 */
    0xE8, 0, 0, 0, 0,                                                 /* 24 call 0x173B8D0 (rel32 at 25) */
    0x48, 0x89, 0x45, 0x60,                                           /* 29 mov [rbp+0x60], rax (the table) */
    0x48, 0x89, 0xC1,                                                 /* 33 mov rcx, rax */
    0x45, 0x89, 0xE0, 0x41, 0xFF, 0xC0, 0x47, 0x8D, 0x04, 0x40, 0x41, 0xC1, 0xE0, 0x02, /* 36 r8d = (route cells + 1) x 12 */
    0x31, 0xD2,                                                       /* 50 xor edx, edx */
    0xE8, 0, 0, 0, 0,                                                 /* 52 call memset (rel32 at 53) */
    0xE9, 0, 0, 0, 0};                                                /* 57 jmp 0xF4C1E2 (rel32 at 58) */
/* Diagnostic (small cities on larger maps): the clear stub ends through kZoneNoteStub (the table, the pass record, the
 * level, the route cell count into g_zoneDiag), the height read starts with kZoneCellStub (the cell's group marker and
 * info cursors); DifficultyWatch writes zones-<n>.tsv (cell types, groups, heights) once the pass is done. */
static const uint8_t kZoneNoteStub[53] = {
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /*  0 mov rax, g_zoneDiag (imm64 at 2) */
    0x48, 0x8B, 0x4D, 0x60, 0x48, 0x89, 0x08,                         /* 10 mov rcx, [rbp+0x60]; mov [rax], rcx (the table) */
    0x4C, 0x89, 0x78, 0x08,                                           /* 17 mov [rax+8], r15 (the pass record) */
    0x48, 0x8B, 0x4E, 0x18, 0x48, 0x89, 0x48, 0x10,                   /* 21 mov rcx, [rsi+0x18]; mov [rax+0x10], rcx (the level) */
    0x44, 0x89, 0x60, 0x18,                                           /* 29 mov [rax+0x18], r12d (route cells) */
    0x49, 0x8B, 0x0F, 0x48, 0x89, 0x48, 0x30,                         /* 33 mov rcx, [r15]; mov [rax+0x30], rcx (the cells' info) */
    0x49, 0x8B, 0x4F, 0x20, 0x48, 0x89, 0x48, 0x38,                   /* 40 mov rcx, [r15+0x20]; mov [rax+0x38], rcx (the cell types) */
    0xE9, 0, 0, 0, 0};                                                /* 48 jmp 0xF4C1E2 (rel32 at 49) */
static const uint8_t kZoneCellStub[23] = {
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /*  0 mov rax, g_zoneDiag (imm64 at 2) */
    0x4C, 0x89, 0x70, 0x20,                                           /* 10 mov [rax+0x20], r14 (the cell's marker + 4) */
    0x48, 0x89, 0x58, 0x28,                                           /* 14 mov [rax+0x28], rbx (its info + 0x18) */
    0xE9, 0, 0, 0, 0};                                                /* 18 jmp kZoneHeightStub (rel32 at 19) */
static volatile uint64_t g_zoneDiag[8];
static const uint8_t kZoneHeightStub[16] = {
    0x48, 0x8B, 0x45, 0x60,                                           /*  0 mov rax, [rbp+0x60] (the table) */
    0xF3, 0x44, 0x0F, 0x10, 0x4C, 0x88, 0x08,                         /*  4 movss xmm9, [rax+rcx*4+8] */
    0xE9, 0, 0, 0, 0};                                                /* 11 jmp 0xF4C3ED (rel32 at 12) */
static const uint8_t kCellSpacingPin[7] = {0xF3, 0x0F, 0x5E, 0xD0, 0x48, 0x8B, 0x12};
static const uint8_t kCellSpacingStub[52] = {
    0xF3, 0x0F, 0x5E, 0xD0,                                           /*  0 divss xmm2, xmm0 */
    0x50,                                                             /*  4 push rax */
    0x48, 0x8B, 0x42, 0x18,                                           /*  5 mov rax, [rdx+0x18] */
    0x81, 0xB8, 0x7C, 0x2D, 0x00, 0x00, 0, 0, 0, 0,                   /*  9 cmp dword [rax+0x2d7c], disk / 1024 (at 15) */
    0x58,                                                             /* 19 pop rax */
    0x75, 0x16,                                                       /* 20 jne 44 */
    0xF3, 0x0F, 0x59, 0x15, 0, 0, 0, 0,                               /* 22 mulss xmm2, [rip+factor x 4] (rel32 at 26) */
    0x66, 0x0F, 0x3A, 0x0A, 0xD2, 0x02,                               /* 30 roundss xmm2, xmm2, ceil */
    0xF3, 0x0F, 0x59, 0x15, 0, 0, 0, 0,                               /* 36 mulss xmm2, [rip+0.25] (rel32 at 40) */
    0x48, 0x8B, 0x12,                                                 /* 44 mov rdx, [rdx] */
    0xE9, 0, 0, 0, 0};                                                /* 47 jmp back (rel32 at 48) */
static const uint8_t kGridPin[9] = {0x4C, 0x89, 0x74, 0x24, 0x20, 0xF3, 0x0F, 0x10, 0x10};
/* Level init (0xF71220) gives a war-table preview (lg+0x2D48 set, at 0xF715BE) a view rect of (0.175, 0.175, 0.65,
 * 0.65) of the world (0xF7178C), +-666 m around the centre, missions (0, 0, 1, 1): lg+0x2D94 (0xF717A8), which every
 * render pass of the generator copies into its command, so the preview's terrain (the mission card's minimap and the
 * war table's hologram) was drawn only in that square, and a larger map's corners were cut off. The radii stub, for the
 * added level's larger maps, also writes the missions' (0, 0, 1, 1) there; level 10 never takes the branch. */
#define DIF_PREVIEW_RECT_SITE 0xF717A8u
static const uint8_t kPreviewRectPin[8] = {0x41, 0x0F, 0x11, 0x85, 0x94, 0x2D, 0x00, 0x00}; /* movups [r13+0x2d94], xmm0 */
static const uint8_t kRadiiStub[116] = {
    0x4C, 0x89, 0x2D, 0, 0, 0, 0,                                     /*  0 mov [rip+watch+0x20], r13 (the level; rel32 at 3) */
    0x49, 0x89, 0x95, 0xE0, 0x2D, 0x00, 0x00,                         /*  7 mov [r13+0x2de0], rdx */
    0x41, 0x80, 0xBD, 0xF9, 0x00, 0x00, 0x00, DIF_VANILLA + 1,        /* 14 cmp byte [r13+0xf9], 11 (the difficulty) */
    0x75, 0x57,                                                       /* 22 jne 111 */
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90,                               /* 24 (every map type) */
    0x41, 0xC7, 0x85, 0x78, 0x2D, 0x00, 0x00, 0, 0, 0, 0,             /* 30 mov dword [r13+0x2d78], fade / 1024 (at 37) */
    0x41, 0xC7, 0x85, 0x7C, 0x2D, 0x00, 0x00, 0, 0, 0, 0,             /* 41 mov dword [r13+0x2d7c], disk / 1024 (at 48) */
    0x41, 0xC7, 0x85, 0x84, 0x2D, 0x00, 0x00, 0, 0, 0, 0,             /* 52 mov dword [r13+0x2d84], rect low (at 59) */
    0x41, 0xC7, 0x85, 0x88, 0x2D, 0x00, 0x00, 0, 0, 0, 0,             /* 63 mov dword [r13+0x2d88], rect high (at 70) */
    0x41, 0xC7, 0x85, 0x8C, 0x2D, 0x00, 0x00, 0, 0, 0, 0,             /* 74 mov dword [r13+0x2d8c], disk + 1 (at 81) */
    0x31, 0xC0,                                                       /* 85 xor eax, eax */
    0x49, 0x89, 0x85, 0x94, 0x2D, 0x00, 0x00,                         /* 87 mov [r13+0x2d94], rax (the view rect: x, y 0) */
    0x48, 0xB8, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x80, 0x3F,       /* 94 mov rax, (1.0, 1.0) */
    0x49, 0x89, 0x85, 0x9C, 0x2D, 0x00, 0x00,                         /* 104 mov [r13+0x2d9c], rax (width, height 1) */
    0xE9, 0, 0, 0, 0};                                                /* 111 jmp back (rel32 at 112) */
/* The preview record copies the convoy route's vertices (G+0x8E9128, count G+0x11A71B8, up to 256) into its own list
 * (record +0x2644, 80 places, then its count at +0x28CC) without a check (0x1003D57): a larger map's longer route ran
 * over the count. The count is capped at 80 where it is stored. */
/* City buildings (generate_city_roads, op 0xA7, 0xFA9E10): the pass record's
 * footprint list (0xFA8FE0: 0x800 x 0x60 from the generation arena, capacity at +0x284) takes a footprint per road edge
 * first (~1250), then the city cells' buildings in cell order, and the cell loop stops when it would overflow (0xFAAED1):
 * later cells keep their flattened plot (flatten_city_regions) with no buildings, mostly a larger map's outer ones (cells
 * grow outward from a random seed). For larger maps the list has 0x2000 places and the per-cell list 0x800 (was 0x200).
 * {rva, instruction length, value at, width, pin, new value}. */
static const struct { uint32_t rva; uint8_t length, at, width; uint8_t pin[10]; uint32_t value; } kCityCapacities[7] = {
    {0xFA904Du, 5, 1, 4, {0xBA, 0x00, 0x00, 0x03, 0x00}, 0xC0000u},                                     /* allocation: 0x800 x 0x60 */
    {0xFA909Fu, 10, 6, 4, {0xC7, 0x81, 0x84, 0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}, 0x2000u},       /* capacity (+0x284) */
    {0xFA90BEu, 6, 2, 4, {0x41, 0xB8, 0x00, 0x00, 0x03, 0x00}, 0xC0000u},                               /* clear size */
    {0xFAAD3Eu, 5, 1, 4, {0xBA, 0x00, 0xC0, 0x00, 0x00}, 0x30000u},                                     /* per-cell list: allocation */
    {0xFAAD4Du, 6, 2, 4, {0x41, 0xB8, 0x00, 0xC0, 0x00, 0x00}, 0x30000u},                               /* clear size */
    {0xFAAD67u, 7, 3, 4, {0xC7, 0x45, 0x04, 0x00, 0x02, 0x00, 0x00}, 0x800u},                           /* capacity */
    {0xFAAD84u, 7, 3, 4, {0x41, 0x81, 0xF8, 0x00, 0x02, 0x00, 0x00}, 0x800u}};                          /* its check */
/* Diagnostic: the footprint list's check (0xFAAECE `cmp edi, [rbp-0x10]; ja 0xFAAC94`, edi = the count with the cell's
 * entries) goes through a stub that keeps the most used and counts the overflows (the cell loop's early exit), which
 * DifficultyWatch logs: whether a level-11 city still loses its outer cells' buildings. Same behaviour otherwise. */
#define DIF_FOOTPRINT_CHECK 0xFAAECEu
#define DIF_FOOTPRINT_FITS 0xFAAED7u
#define DIF_FOOTPRINT_OVER 0xFAAC94u
static const uint8_t kFootprintPin[9] = {0x3B, 0x7D, 0xF0, 0x0F, 0x87, 0xBD, 0xFD, 0xFF, 0xFF};
static const uint8_t kFootprintStub[44] = {
    0x3B, 0x7D, 0xF0,                                                 /*  0 cmp edi, [rbp-0x10] (the capacity) */
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /*  3 mov rax, g_cityFootprints (imm64 at 5) */
    0x77, 0x0B,                                                       /* 13 ja 26 */
    0x3B, 0x38, 0x76, 0x02, 0x89, 0x38,                               /* 15 cmp edi, [rax]; jbe 21; mov [rax], edi (most used) */
    0xE9, 0, 0, 0, 0,                                                 /* 21 jmp 0xFAAED7 (rel32 at 22) */
    0xF0, 0xFF, 0x40, 0x04,                                           /* 26 lock inc dword [rax+4] (overflows) */
    0x8B, 0x4D, 0xF0, 0x89, 0x48, 0x08,                               /* 30 mov ecx, [rbp-0x10]; mov [rax+8], ecx */
    0x89, 0x78, 0x0C,                                                 /* 36 mov [rax+0xc], edi (wanted) */
    0xE9, 0, 0, 0, 0};                                                /* 39 jmp 0xFAAC94 (rel32 at 40) */
static volatile LONG g_cityFootprints[4]; /* most used, overflows, capacity, wanted */
#define DIF_CONVOY_SITE 0x1003D57u
#define DIF_CONVOY_BACK 0x1003D63u
static const uint8_t kConvoyPin[12] = {0x8B, 0x88, 0xB8, 0x71, 0x1A, 0x01, 0x89, 0x8E, 0xCC, 0x28, 0x00, 0x00};
static const uint8_t kConvoyStub[27] = {
    0x8B, 0x88, 0xB8, 0x71, 0x1A, 0x01,                               /*  0 mov ecx, [rax+0x11a71b8] */
    0x83, 0xF9, 0x50,                                                 /*  6 cmp ecx, 80 */
    0x76, 0x05,                                                       /*  9 jbe 16 */
    0xB9, 0x50, 0x00, 0x00, 0x00,                                     /* 11 mov ecx, 80 */
    0x89, 0x8E, 0xCC, 0x28, 0x00, 0x00,                               /* 16 mov [rsi+0x28cc], ecx */
    0xE9, 0, 0, 0, 0};                                                /* 22 jmp 0x1003D63 (rel32 at 23) */
/* The spacing reads: the level's disk (lg+0x2D7C) is the one the stub above wrote, so the spacing is the level's. */
static const uint8_t kSpacingStub[34] = {
    0xF3, 0x0F, 0x10, 0x50, 0x04,                                     /*  0 movss xmm2, [rax+4] */
    0x49, 0x8B, 0x46, 0x18,                                           /*  5 mov rax, [r14+0x18] */
    0x81, 0xB8, 0x7C, 0x2D, 0x00, 0x00, 0, 0, 0, 0,                   /*  9 cmp dword [rax+0x2d7c], disk / 1024 (at 15) */
    0x75, 0x08,                                                       /* 19 jne 29 */
    0xF3, 0x0F, 0x10, 0x15, 0, 0, 0, 0,                               /* 21 movss xmm2, [rip+spacing] (rel32 at 25) */
    0xE9, 0, 0, 0, 0};                                                /* 29 jmp back (rel32 at 30) */
static const uint8_t kSpacingStub2[46] = {
    0x51,                                                             /*  0 push rcx */
    0x48, 0x8B, 0x4C, 0x24, 0x68,                                     /*  1 mov rcx, [rsp+0x68] */
    0x48, 0x8B, 0x49, 0x18,                                           /*  6 mov rcx, [rcx+0x18] */
    0x81, 0xB9, 0x7C, 0x2D, 0x00, 0x00, 0, 0, 0, 0,                   /* 10 cmp dword [rcx+0x2d7c], disk / 1024 (at 16) */
    0x59,                                                             /* 20 pop rcx */
    0x75, 0x0D,                                                       /* 21 jne 36 */
    0xF3, 0x0F, 0x5C, 0x35, 0, 0, 0, 0,                               /* 23 subss xmm6, [rip+spacing] (rel32 at 27) */
    0xE9, 0, 0, 0, 0,                                                 /* 31 jmp back (rel32 at 32) */
    0xF3, 0x0F, 0x5C, 0x70, 0x04,                                     /* 36 subss xmm6, [rax+4] */
    0xE9, 0, 0, 0, 0};                                                /* 41 jmp back (rel32 at 42) */
static const uint8_t kGridStub[38] = {
    0x4C, 0x89, 0x74, 0x24, 0x20,                                     /*  0 mov [rsp+0x20], r14 */
    0xF3, 0x0F, 0x10, 0x10,                                           /*  5 movss xmm2, [rax] */
    0x48, 0x8B, 0x46, 0x18,                                           /*  9 mov rax, [rsi+0x18] */
    0x81, 0xB8, 0x7C, 0x2D, 0x00, 0x00, 0, 0, 0, 0,                   /* 13 cmp dword [rax+0x2d7c], disk / 1024 (at 19) */
    0x75, 0x08,                                                       /* 23 jne 33 */
    0xF3, 0x0F, 0x10, 0x15, 0, 0, 0, 0,                               /* 25 movss xmm2, [rip+grid] (rel32 at 29) */
    0xE9, 0, 0, 0, 0};                                                /* 33 jmp back (rel32 at 34) */

/* ---- the checks ------------------------------------------------------------------------------------------------- */

static const char* SpreadFailed(uintptr_t game)
{
    uint8_t board[7];
    if (SafeRead(game + DIF_SPREAD_REFERENCE, board, 7) || memcmp(board, kSpreadReferencePin, 7) != 0)
        return "the map generator's location spreading is not the reviewed one";
    return NULL;
}

static const char* HashInsertFailed(uintptr_t game)
{
    uint8_t insertEnd[sizeof kHashInsertEndPin];
    if (SafeRead(game + DIF_HASH_INSERT_END, insertEnd, sizeof insertEnd) || memcmp(insertEnd, kHashInsertEndPin, sizeof insertEnd) != 0)
        return "the game's hash insert is not the reviewed one";
    return NULL;
}

/* A larger map's generator sites (radii, spacing, grid, route cells), height zones and city capacities. */
static const char* MapPinsFailed(uintptr_t game)
{
    if (!LargerMaps()) return NULL;
    uint8_t pin[9];
    if (SafeRead(game + DIF_MAP_TYPE_SITE, pin, 7) || memcmp(pin, kMapTypePin, 7) != 0 ||
        SafeRead(game + DIF_SPACING_SITE, pin, 5) || memcmp(pin, kSpacingPin, 5) != 0 ||
        SafeRead(game + DIF_SPACING_SITE_2, pin, 5) || memcmp(pin, kSpacingPin2, 5) != 0 ||
        SafeRead(game + DIF_GRID_SITE, pin, 9) || memcmp(pin, kGridPin, 9) != 0 ||
        SafeRead(game + DIF_CELL_SPACING_SITE, pin, 7) || memcmp(pin, kCellSpacingPin, 7) != 0 ||
        SafeRead(game + DIF_FINALIZE_SPACING_SITE, pin, 9) || memcmp(pin, kFinalizeSpacingPin, 9) != 0)
        return "the map generator is not the reviewed one (a map_scale above 1 needs it)";
    uint8_t zone[21];
    if (SafeRead(game + DIF_ZONE_SITE, zone, 14) || memcmp(zone, kZonePin, 14) != 0 ||
        SafeRead(game + DIF_ZONE_CLEAR, zone, 21) || memcmp(zone, kZoneClearPin, 21) != 0 ||
        SafeRead(game + DIF_ZONE_TABLE_LEA, zone, 8) || memcmp(zone, kZoneTableLeaPin, 8) != 0 ||
        SafeRead(game + DIF_ZONE_HEIGHT, zone, 7) || memcmp(zone, kZoneHeightPin, 7) != 0)
        return "the map generator is not the reviewed one (a map_scale above 1 needs it)";
    for (int i = 0; i < 7; i++)
        if (SafeRead(game + kCityCapacities[i].rva, zone, kCityCapacities[i].length) || memcmp(zone, kCityCapacities[i].pin, kCityCapacities[i].length) != 0)
            return "the city generator is not the reviewed one (a map_scale above 1 needs it)";
    if (SafeRead(game + DIF_FOOTPRINT_CHECK, zone, 9) || memcmp(zone, kFootprintPin, 9) != 0)
        return "the city generator is not the reviewed one (a map_scale above 1 needs it)";
    return NULL;
}

/* A larger map's preview view rect and convoy route, and the stamp index. */
static const char* PreviewFailed(uintptr_t game)
{
    if (!LargerMaps()) return NULL;
    uint8_t zone[21], pin[9];
    if (SafeRead(game + DIF_PREVIEW_RECT_SITE, zone, 8) || memcmp(zone, kPreviewRectPin, 8) != 0 ||
        SafeRead(game + DIF_CONVOY_SITE, zone, 12) || memcmp(zone, kConvoyPin, 12) != 0)
        return "the war-table preview's view or convoy route is not the reviewed one (a map_scale above 1 needs it)";
    for (int i = 0; i < 5; i++)
        if (SafeRead(game + kStampIndex[i].rva, pin, kStampIndex[i].length) || memcmp(pin, kStampIndex[i].pin, kStampIndex[i].length) != 0)
            return "the map generator's stamp index is not the reviewed one (a map_scale above 1 needs it)";
    return NULL;
}

/* ---- the install steps ------------------------------------------------------------------------------------------ */

/* The city copies' installer (kStrongholdStub), when the level wants city strongholds, ICBM missions or the radar: the
 * radii stub ends through it. */
static const char* PrepareStrongholds(DifficultyInstall* d)
{
    if (!(g_difficulty.flags & (HF_DIFFICULTY_CITY_STRONGHOLDS | HF_DIFFICULTY_CITY_ICBM | HF_DIFFICULTY_RADAR))) return NULL;
    uintptr_t game = d->game;
    int32_t rel;
    g_cityCopyFlags = g_difficulty.flags & (HF_DIFFICULTY_CITY_STRONGHOLDS | HF_DIFFICULTY_CITY_ICBM | HF_DIFFICULTY_RADAR);
    uint8_t* strongholds = AllocateNearGame(game, 0x100);
    if (strongholds == NULL) return "no memory within 2 GB of game.dll for the city strongholds";
    uint64_t install = (uint64_t)(uintptr_t)(void*)StrongholdInstall;
    memcpy(strongholds, kStrongholdStub, 125);
    memcpy(strongholds + 60, &install, 8);
    strongholds[125] = 0xE9;                                  /* jmp back after the level init's store */
    rel = (int32_t)(game + DIF_MAP_TYPE_SITE + 7 - (uintptr_t)(strongholds + 130)); memcpy(strongholds + 126, &rel, 4);
    DWORD strongholdProtect;
    VirtualProtect(strongholds, 0x100, PAGE_EXECUTE_READ, &strongholdProtect);
    rel = (int32_t)(strongholds - (d->radiiStub + 116)); memcpy(d->radiiStub + 112, &rel, 4); /* the radii stub ends through it */
    return NULL;
}

/* The convoy route's cap, both spacing reads and the Voronoi grid, for a disk of diskBits (x 1024 m). */
static void PrepareSpacingStubs(DifficultyInstall* d, float diskBits)
{
    uintptr_t game = d->game;
    int32_t rel;
    memcpy(d->convoyStub, kConvoyStub, sizeof kConvoyStub);
    rel = (int32_t)(game + DIF_CONVOY_BACK - (uintptr_t)(d->convoyStub + 27)); memcpy(d->convoyStub + 23, &rel, 4);
    memcpy(d->spacingStub, kSpacingStub, sizeof kSpacingStub);
    memcpy(d->spacingStub + 15, &diskBits, 4);
    rel = (int32_t)((uint8_t*)d->spacingValue - (d->spacingStub + 29)); memcpy(d->spacingStub + 25, &rel, 4);
    rel = (int32_t)(game + DIF_SPACING_SITE + 5 - (uintptr_t)(d->spacingStub + 34)); memcpy(d->spacingStub + 30, &rel, 4);
    memcpy(d->spacingStub2, kSpacingStub2, sizeof kSpacingStub2);
    memcpy(d->spacingStub2 + 16, &diskBits, 4);
    rel = (int32_t)((uint8_t*)d->spacingValue - (d->spacingStub2 + 31)); memcpy(d->spacingStub2 + 27, &rel, 4);
    rel = (int32_t)(game + DIF_SPACING_SITE_2 + 5 - (uintptr_t)(d->spacingStub2 + 36)); memcpy(d->spacingStub2 + 32, &rel, 4);
    rel = (int32_t)(game + DIF_SPACING_SITE_2 + 5 - (uintptr_t)(d->spacingStub2 + 46)); memcpy(d->spacingStub2 + 42, &rel, 4);
    *d->gridValue = ceilf(16.0f * (d->disk + 1.0f) / 681.0f * 1.015f * 4.0f) / 4.0f;
    memcpy(d->gridStub, kGridStub, sizeof kGridStub);
    memcpy(d->gridStub + 19, &diskBits, 4);
    rel = (int32_t)((uint8_t*)d->gridValue - (d->gridStub + 33)); memcpy(d->gridStub + 29, &rel, 4);
    rel = (int32_t)(game + DIF_GRID_SITE + 9 - (uintptr_t)(d->gridStub + 38)); memcpy(d->gridStub + 34, &rel, 4);
}

/* A larger map's radii and spacing (m, from the centre): border R = playable + 174 (settings +0x48), fade R + 57 and
 * disk R + 117 as at level 10 (620 / 680), the disk within the world; the spacing keeps level 10's sites,
 * 0.8 R^2 + 0.2 (disk + 1)^2 against 0.8 x 563.12^2 + 0.2 x 681^2 (sites past R are thinned to a fifth). */
static const char* PrepareMapStubs(DifficultyInstall* d)
{
    uintptr_t game = d->game;
    int32_t rel;
    d->radiiStub = d->code + 0xC40; /* 116 bytes */
    d->convoyStub = d->code + 0xCC0;
    d->spacingStub = d->code + 0x460;
    d->spacingStub2 = d->code + 0x490;
    d->spacingValue = (float*)(d->code + 0x4C0);
    d->gridValue = (float*)(d->code + 0x4C4);
    d->gridStub = d->code + 0x4D0;
    d->disk = 0.0f;
    if (!LargerMaps()) return NULL;
    uint8_t* radiiStub = d->radiiStub;
    float border = 389.12f * g_difficulty.mapScale + 174.0f;
    float disk = border + 117.0f < 1022.0f ? border + 117.0f : 1022.0f;
    d->disk = disk;
    float fade = border + 57.0f < disk ? border + 57.0f : disk;
    float radii[5] = {fade / 1024.0f, disk / 1024.0f, 1024.0f - disk - 1.0f, 1024.0f + disk + 1.0f, disk + 1.0f};
    /* In quarter metres: map type 1 lays the route sites as a grid (0xC126C0 cl=1), and a grid of inexact spacing
     * splits its 4-way corners (65.7 m: 1004 vertices for 752 sites, past the 975; 65.75 m: 815). */
    *d->spacingValue = ceilf(160.0f * sqrtf((0.8f * border * border + 0.2f * (disk + 1.0f) * (disk + 1.0f)) /
        (0.8f * 563.12f * 563.12f + 0.2f * 681.0f * 681.0f))) / 4.0f;
    memcpy(radiiStub, kRadiiStub, sizeof kRadiiStub);
    for (int i = 0; i < 5; i++) memcpy(radiiStub + 37 + 11 * i, &radii[i], 4);
    rel = (int32_t)(d->watch + 0x20 - (radiiStub + 7)); memcpy(radiiStub + 3, &rel, 4);
    rel = (int32_t)(game + DIF_MAP_TYPE_SITE + 7 - (uintptr_t)(radiiStub + 116)); memcpy(radiiStub + 112, &rel, 4);
    const char* why = PrepareStrongholds(d);
    if (why != NULL) return why;
    PrepareSpacingStubs(d, radii[1]);
    return NULL;
}

/* A larger map's route cell and FinalizeVoronoi spacing stubs, and their factor (x 4, then 0.25). */
static const char* PrepareCellStubs(DifficultyInstall* d)
{
    uintptr_t game = d->game;
    int32_t rel;
    float* cellFactor = (float*)(d->code + 0x5C0);
    uint8_t* cellStub = d->code + 0x5D0;
    uint8_t* finalizeStub = d->code + 0x610;
    d->cellFactor = cellFactor;
    d->cellStub = cellStub;
    d->finalizeStub = finalizeStub;
    if (!LargerMaps()) return NULL;
    cellFactor[0] = (d->disk + 1.0f) / 681.0f * 1.04f * 4.0f;
    cellFactor[1] = 0.25f;
    float diskBits = d->disk / 1024.0f;
    memcpy(cellStub, kCellSpacingStub, sizeof kCellSpacingStub);
    memcpy(cellStub + 15, &diskBits, 4);
    rel = (int32_t)((uint8_t*)&cellFactor[0] - (cellStub + 30)); memcpy(cellStub + 26, &rel, 4);
    rel = (int32_t)((uint8_t*)&cellFactor[1] - (cellStub + 44)); memcpy(cellStub + 40, &rel, 4);
    rel = (int32_t)(game + DIF_CELL_SPACING_SITE + 7 - (uintptr_t)(cellStub + 52)); memcpy(cellStub + 48, &rel, 4);
    memcpy(finalizeStub, kFinalizeSpacingStub, sizeof kFinalizeSpacingStub);
    memcpy(finalizeStub + 24, &diskBits, 4);
    rel = (int32_t)((uint8_t*)&cellFactor[0] - (finalizeStub + 38)); memcpy(finalizeStub + 34, &rel, 4);
    rel = (int32_t)((uint8_t*)&cellFactor[1] - (finalizeStub + 52)); memcpy(finalizeStub + 48, &rel, 4);
    rel = (int32_t)(game + DIF_FINALIZE_SPACING_SITE + 9 - (uintptr_t)(finalizeStub + 57)); memcpy(finalizeStub + 53, &rel, 4);
    return NULL;
}

/* A larger map's height zone stubs: the clear (+0, ending through the note +0x80), the height (+0x40, entered through
 * the cell note +0xC0). */
static const char* PrepareZoneStubs(DifficultyInstall* d)
{
    uintptr_t game = d->game;
    int32_t rel;
    uint8_t* zoneStubs = LargerMaps() ? AllocateNearGame(game, 0x100) : NULL;
    d->zoneStubs = zoneStubs;
    if (!LargerMaps()) return NULL;
    if (zoneStubs == NULL) return "no memory within 2 GB of game.dll for the height zones";
    memcpy(zoneStubs, kZoneClearStub, sizeof kZoneClearStub);
    rel = (int32_t)(game + DIF_ARENA_ALLOCATE - (uintptr_t)(zoneStubs + 29)); memcpy(zoneStubs + 25, &rel, 4);
    rel = (int32_t)(game + DIF_MEMSET - (uintptr_t)(zoneStubs + 57)); memcpy(zoneStubs + 53, &rel, 4);
    rel = (int32_t)(zoneStubs + 0x80 - (zoneStubs + 62)); memcpy(zoneStubs + 58, &rel, 4);
    memcpy(zoneStubs + 0x40, kZoneHeightStub, sizeof kZoneHeightStub);
    rel = (int32_t)(game + DIF_ZONE_HEIGHT_BACK - (uintptr_t)(zoneStubs + 0x40 + 16)); memcpy(zoneStubs + 0x40 + 12, &rel, 4);
    uint64_t diag = (uint64_t)(uintptr_t)g_zoneDiag;
    memcpy(zoneStubs + 0x80, kZoneNoteStub, sizeof kZoneNoteStub); memcpy(zoneStubs + 0x80 + 2, &diag, 8);
    rel = (int32_t)(game + DIF_ZONE_CLEAR_BACK - (uintptr_t)(zoneStubs + 0x80 + 53)); memcpy(zoneStubs + 0x80 + 49, &rel, 4);
    memcpy(zoneStubs + 0xC0, kZoneCellStub, sizeof kZoneCellStub); memcpy(zoneStubs + 0xC0 + 2, &diag, 8);
    rel = (int32_t)(zoneStubs + 0x40 - (zoneStubs + 0xC0 + 23)); memcpy(zoneStubs + 0xC0 + 19, &rel, 4);
    DWORD zoneProtect;
    VirtualProtect(zoneStubs, 0x100, PAGE_EXECUTE_READ, &zoneProtect);
    return NULL;
}

/* SpreadLocations' reference: location 0 when none qualifies, through its own stub. */
static const char* InstallSpreadGuard(DifficultyInstall* d)
{
    uintptr_t game = d->game;
    uint8_t* spread = AllocateNearGame(game, 0x100);
    if (spread == NULL) return "no memory within 2 GB of game.dll for the location spreading";
    memcpy(spread, kSpreadReferenceStub, sizeof kSpreadReferenceStub);
    int32_t rel = (int32_t)(game + DIF_SPREAD_REFERENCE_BACK - (uintptr_t)(spread + 24)); memcpy(spread + 20, &rel, 4);
    DWORD spreadProtect;
    VirtualProtect(spread, 0x100, PAGE_EXECUTE_READ, &spreadProtect);
    uint8_t spreadJump[10] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
    rel = (int32_t)((uintptr_t)spread - (game + DIF_SPREAD_REFERENCE + 5)); memcpy(spreadJump + 1, &rel, 4);
    if (ApiWrite(game + DIF_SPREAD_REFERENCE, spreadJump, 10)) return "the location spreading could not be guarded";
    return NULL;
}

/* The hash insert's end: a full table skips the entry and counts it, through its own stub. */
static const char* InstallHashInsertGuard(DifficultyInstall* d)
{
    uintptr_t game = d->game;
    uint8_t* insertEnd = AllocateNearGame(game, 0x2000); /* the stub's page (+0), its count's page (+0x1000, stays writable) */
    if (insertEnd == NULL) return "no memory within 2 GB of game.dll for the hash insert's guard";
    memcpy(insertEnd, kHashInsertEndStub, sizeof kHashInsertEndStub);
    g_hashFullCount = (volatile LONG*)(insertEnd + 0x1000);
    DWORD insertProtect;
    VirtualProtect(insertEnd, 0x1000, PAGE_EXECUTE_READ, &insertProtect);
    uint8_t insertJump[12] = {0xE9, 0, 0, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    int32_t rel = (int32_t)((uintptr_t)insertEnd - (game + DIF_HASH_INSERT_END + 5)); memcpy(insertJump + 1, &rel, 4);
    if (ApiWrite(game + DIF_HASH_INSERT_END, insertJump, 12)) return "the game's hash insert could not be guarded";
    return NULL;
}

/* A larger map's generator sites into their stubs, and the stamp index enlarged. */
static const char* JumpToMapStubs(DifficultyInstall* d)
{
    if (!LargerMaps()) return NULL;
    uintptr_t game = d->game;
    const struct { uint32_t rva, length; uint8_t* stub; } mapSites[6] = {{DIF_SPACING_SITE, 5, d->spacingStub},
        {DIF_SPACING_SITE_2, 5, d->spacingStub2}, {DIF_GRID_SITE, 9, d->gridStub}, {DIF_CELL_SPACING_SITE, 7, d->cellStub},
        {DIF_FINALIZE_SPACING_SITE, 9, d->finalizeStub}, {DIF_MAP_TYPE_SITE, 7, d->radiiStub}};
    for (size_t i = 0; i < 6; i++)
    {
        uint8_t jump[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
        int32_t rel = (int32_t)((uintptr_t)mapSites[i].stub - (game + mapSites[i].rva + 5));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + mapSites[i].rva, jump, mapSites[i].length)) return "the map generator could not be given the larger map";
    }
    for (int i = 0; i < 5; i++)
        if (ApiWrite(game + kStampIndex[i].rva, kStampIndex[i].bytes, kStampIndex[i].length))
            return "the map generator's stamp index could not be enlarged";
    return NULL;
}

/* A larger map's height zones' table moved, the preview's convoy route limited and the city generator enlarged. */
static const char* MoveZoneTable(DifficultyInstall* d)
{
    if (!LargerMaps()) return NULL;
    uintptr_t game = d->game;
    int32_t rel;
    uint8_t jump[14];
    memset(jump, 0x90, sizeof jump);
    jump[0] = 0xE9;
    uint8_t clear[21], height[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90}, load = 0x8B;
    memset(clear, 0x90, sizeof clear);
    clear[0] = 0xE9;
    rel = (int32_t)((uintptr_t)d->zoneStubs - (game + DIF_ZONE_CLEAR + 5)); memcpy(clear + 1, &rel, 4);
    rel = (int32_t)((uintptr_t)(d->zoneStubs + 0xC0) - (game + DIF_ZONE_HEIGHT + 5)); memcpy(height + 1, &rel, 4);
    if (ApiWrite(game + DIF_ZONE_CLEAR, clear, sizeof clear) || ApiWrite(game + DIF_ZONE_TABLE_LEA + 1, &load, 1) ||
        ApiWrite(game + DIF_ZONE_HEIGHT, height, sizeof height))
        return "the height zones' table could not be moved";
    memset(jump, 0x90, sizeof jump);
    jump[0] = 0xE9;
    rel = (int32_t)((uintptr_t)d->convoyStub - (game + DIF_CONVOY_SITE + 5));
    memcpy(jump + 1, &rel, 4);
    if (ApiWrite(game + DIF_CONVOY_SITE, jump, 12)) return "the preview's convoy route could not be limited";
    for (int i = 0; i < 7; i++)
        if (ApiWrite(game + kCityCapacities[i].rva + kCityCapacities[i].at, &kCityCapacities[i].value, kCityCapacities[i].width))
            return "the city generator could not be enlarged";
    return NULL;
}

/* A larger map's city diagnostic: the footprint list's check through its stub (g_cityFootprints). */
static const char* InstallFootprintWatch(DifficultyInstall* d)
{
    if (!LargerMaps()) return NULL;
    uintptr_t game = d->game;
    int32_t rel;
    uint8_t* footprints = AllocateNearGame(game, 0x100);
    if (footprints == NULL) return "no memory within 2 GB of game.dll for the city diagnostic";
    uint64_t counters = (uint64_t)(uintptr_t)g_cityFootprints;
    memcpy(footprints, kFootprintStub, sizeof kFootprintStub);
    memcpy(footprints + 5, &counters, 8);
    rel = (int32_t)(game + DIF_FOOTPRINT_FITS - (uintptr_t)(footprints + 26)); memcpy(footprints + 22, &rel, 4);
    rel = (int32_t)(game + DIF_FOOTPRINT_OVER - (uintptr_t)(footprints + 44)); memcpy(footprints + 40, &rel, 4);
    DWORD footprintProtect;
    VirtualProtect(footprints, 0x100, PAGE_EXECUTE_READ, &footprintProtect);
    uint8_t check[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
    rel = (int32_t)((uintptr_t)footprints - (game + DIF_FOOTPRINT_CHECK + 5)); memcpy(check + 1, &rel, 4);
    if (ApiWrite(game + DIF_FOOTPRINT_CHECK, check, 9)) return "the city generator's check could not be watched";
    return NULL;
}

/* The navigation grid's two stubs (the mission's, +0; the ship's, +0x100) calling NavRetile before the game's own. */
static void PrepareNavStubs(uintptr_t game, uint8_t* nav)
{
    void* function = (void*)NavRetile;
    uint64_t system = game + DIF_NAV_SYSTEM;
    memcpy(nav, kNavStub, sizeof kNavStub);
    memcpy(nav + 60, &function, 8); memcpy(nav + 126, &system, 8);
    int32_t rel = (int32_t)(game + DIF_NAV_BACK - (uintptr_t)(nav + 145)); memcpy(nav + 141, &rel, 4);
    memcpy(nav + 0x100, kNavShipStub, sizeof kNavShipStub);
    memcpy(nav + 0x100 + 65, &function, 8);
    rel = (int32_t)(game + DIF_NAV_SHIP_BACK - (uintptr_t)(nav + 0x100 + 144)); memcpy(nav + 0x100 + 140, &rel, 4);
    DWORD navProtect;
    VirtualProtect(nav, 0x200, PAGE_EXECUTE_READ, &navProtect);
}

/* A larger map's navigation grid: 256 m tiles for its navmesh (NavRetile), when the engine's grid is the reviewed one. */
static const char* InstallNavGrid(DifficultyInstall* d)
{
    if (!LargerMaps()) return NULL;
    uintptr_t game = d->game;
    uintptr_t exe = ApiModuleBase("helldivers2.exe", NULL);
    uint8_t navPin[23];
    uint8_t* nav = AllocateNearGame(game, 0x200);
    if (exe == 0 || SafeRead(exe + DIF_NAV_PIN, navPin, sizeof navPin) || memcmp(navPin, kNavExePin, sizeof navPin) != 0 ||
        SafeRead(game + DIF_NAV_SITE, navPin, 15) || memcmp(navPin, kNavPin, 15) != 0 ||
        SafeRead(game + DIF_NAV_SHIP_SITE, navPin, 10) || memcmp(navPin, kNavShipPin, 10) != 0 || nav == NULL)
    {
        ApiLog(g_difficulty.mod, "difficulty %s: the navigation grid is not the reviewed one; larger maps keep a navmesh of +-512 m (mission stratagems will not stick beyond it)", g_difficulty.name);
        return NULL;
    }
    g_navExe = exe;
    uint8_t read[8];
    float sizes[2] = {0.0f, 0.0f};
    g_navTiles256 = !SafeRead(exe + DIF_NAV_TILE_SIZE_READ, read, 8) && !memcmp(read, kNavTileSizeRead, 8) &&
        !SafeRead(exe + 0x16A9DB0, &sizes[0], 4) && !SafeRead(exe + 0x16AA7C0, &sizes[1], 4) && sizes[0] == 128.0f && sizes[1] == 256.0f;
    if (!g_navTiles256)
        ApiLog(g_difficulty.mod, "difficulty %s: the navmesh's terrain build is not the reviewed one; larger maps keep a navmesh of +-512 m (mission stratagems will not stick beyond it)", g_difficulty.name);
    PrepareNavStubs(game, nav);
    uint8_t navJump[15];
    memset(navJump, 0x90, sizeof navJump);
    navJump[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)nav - (game + DIF_NAV_SITE + 5)); memcpy(navJump + 1, &rel, 4);
    if (ApiWrite(game + DIF_NAV_SITE, navJump, 15)) return "the navigation grid could not be widened";
    rel = (int32_t)((uintptr_t)(nav + 0x100) - (game + DIF_NAV_SHIP_SITE + 5)); memcpy(navJump + 1, &rel, 4);
    if (ApiWrite(game + DIF_NAV_SHIP_SITE, navJump, 10)) return "the navigation grid could not be restored for the ship";
    return NULL;
}
