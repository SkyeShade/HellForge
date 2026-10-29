/* Towns on level 11's larger maps, their counters and two guards (research d11-sparse-cities). Included by
 * hellforge_modloader.c after difficulty_maps.c.
 *
 * The generator fills a city cell with building and road stamps by rule: round(area x coverage / size) of each, so a
 * larger cell gets more. But each rule may place only up to +0x54 metres past the playable radius R: the test is
 * distance - R < rule+0x54, at the five placers below, each multiplying by R (lg+0x2D70, base +0x50). Nearly half the
 * city building rules allow 0 m and most of the rest 30-80 m, so between R and the border (174 m) a town keeps its
 * flattened plot, its roads and its big blocks only; outposts and objectives keep their buildings (their levels spawn
 * whole). At level 10 that band is scenery behind the boundary; on a larger map many towns stand in it, where players
 * walk (InstallCull keeps their units up to the border for the same reason). On a larger map the five placers now
 * measure from the border (lg+0x2D74, base +0x54) as the cull does: rules with 0 m place up to the border. Off with
 * HELLFORGE_D11_TOWN_BORDER=0, for comparing.
 *
 * With it, on every larger map:
 *   - counters: buildings placed (0xF99AAE), positions refused past R and the rule's reach (0xF995E8) and refused with
 *     the location list full (0xF994AE), which DifficultyWatch logs;
 *   - the subregion stamps' merge (op 0x2B, 0xF35030) appended to the 8192-place location list unchecked; past it a
 *     job's remaining results are dropped and counted (as a full list's other appends refuse);
 *   - the destruction-record map's static registration (0x13E0285) probes inline, and when the map is full (a larger
 *     city map fills its 32768 slots) wrote the record through a null slot (0x13E036A): that destructible is now left
 *     without a record and counted, as the shared insert's guard does (DIF_HASH_INSERT_END). */

#define DIF_TOWN_COUNTERS 8 /* placed, refused past R, refused locations full, merge dropped, records skipped; town walls:
                             * no edge in reach, the wrong side, tried */
static volatile LONG* g_townCounters;

/* The five placers' R: {site, length, the instruction (R at +0x50; its last byte 0x50 becomes 0x54, the border), cmp
 * dword [base+0x5c] (the level's disk, lg+0x2D7C), back}. */
static const struct { uint32_t site; uint8_t length; uint8_t pin[6]; uint8_t cmp[3]; uint32_t back; } kTownBorderSites[5] = {
    {0xF9959Au, 6, {0xF3, 0x44, 0x0F, 0x59, 0x41, 0x50}, {0x81, 0x79, 0x5C}, 0xF995A0u}, /* city buildings: mulss xmm8, [rcx+0x50] */
    {0xF340C8u, 6, {0xF3, 0x44, 0x0F, 0x59, 0x4E, 0x50}, {0x81, 0x7E, 0x5C}, 0xF340CEu}, /* subregion stamps: mulss xmm9, [rsi+0x50] */
    {0xF347F5u, 6, {0xF3, 0x44, 0x0F, 0x59, 0x49, 0x50}, {0x81, 0x79, 0x5C}, 0xF347FBu}, /* subregion stamps: mulss xmm9, [rcx+0x50] */
    {0xFB27D7u, 5, {0xF3, 0x0F, 0x59, 0x71, 0x50}, {0x81, 0x79, 0x5C}, 0xFB27DCu},       /* city road stamps: mulss xmm6, [rcx+0x50] */
    {0xF3193Fu, 5, {0xF3, 0x0F, 0x59, 0x79, 0x50}, {0x81, 0x79, 0x5C}, 0xF31944u}};      /* stamp sub-placer: mulss xmm7, [rcx+0x50] */
/* The counted branches: jae to the refusal (0F 83 rel32), {site, its target, counter}. */
static const struct { uint32_t site; uint8_t pin[6]; uint32_t target; uint8_t counter; } kTownRefusals[2] = {
    {0xF994AEu, {0x0F, 0x83, 0x03, 0x06, 0x00, 0x00}, 0xF99AB7u, 2},  /* the location list full */
    {0xF995E8u, {0x0F, 0x83, 0xC9, 0x04, 0x00, 0x00}, 0xF99AB7u, 1}}; /* past R and the rule's reach */
#define DIF_TOWN_PLACED 0xF99AAEu        /* call 0xFC9C10: a building placed */
#define DIF_TOWN_PLACED_CALL 0xFC9C10u
static const uint8_t kTownPlacedPin[5] = {0xE8, 0x5D, 0x01, 0x03, 0x00};
#define DIF_TOWN_MERGE 0xF35030u         /* mov rcx, [rdi+0x20]; mov r10d, r13d: op 0x2B's merge, r13d the list's count */
#define DIF_TOWN_MERGE_DROP 0xF3517Cu    /* the job's remaining results skipped */
static const uint8_t kTownMergePin[7] = {0x48, 0x8B, 0x4F, 0x20, 0x45, 0x8B, 0xD5};
#define DIF_RECORD_STORE 0x13E036Au      /* mov [r8+4], edi; mov [r8], ebx: r8 the slot, 0 when the map is full */
static const uint8_t kRecordStorePin[7] = {0x41, 0x89, 0x78, 0x04, 0x41, 0x89, 0x18};

/* Diagnostic (research d11-sparse-cities §7): a type 0/1 map's town is a group of connected zone cells (op 0x3E,
 * force_flatten_zones), one plateau at min(mid, lowest + raise) of the heights sampled around it, 0 with none sampled.
 * At the end of the grouping loop (0xF4C34B), while the group table is alive (zones-<n>.tsv read it after a later pass
 * had reused it), a stub calls ZoneGroupsCaught: per group its zone type, cells, centre, min, max and height, into
 * zone-groups-<n>.tsv and a line, for the first 16 generations with zone cells at any level (a level-10 one compares;
 * the war table's previews make many without). */
#define DIF_ZONE_GROUPS_SITE 0xF4C34Bu /* mov rdi, [rsp+0x68] (the cells' markers), then 0xF4C350 */
static const uint8_t kZoneGroupsPin[5] = {0x48, 0x8B, 0x7C, 0x24, 0x68};
#define DIF_ZONE_CATCH_GROUPS 64
typedef struct { int32_t type; uint32_t cells; float x, y, min, max, height; } ZoneGroupCaught;
typedef struct { uint8_t difficulty; uint32_t cells, zoneCells, groups; ZoneGroupCaught group[DIF_ZONE_CATCH_GROUPS]; } ZoneCatch;
static ZoneCatch g_zoneCatch; /* the last one, for the fixture */
static volatile LONG g_zoneCatches;
/* push the volatile registers; keep xmm0-5; ZoneGroupsCaught(the function's rsp, rbp, r14d = the groups, rsi = the pass
 * context); restore; the replaced load; jmp 0xF4C350 */
static const uint8_t kZoneGroupsStub[135] = {
    0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,             /*   0 push rax, rcx, rdx, r8-r11 */
    0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00,                                     /*  11 sub rsp, 0x88 */
    0x0F, 0x11, 0x44, 0x24, 0x20, 0x0F, 0x11, 0x4C, 0x24, 0x30, 0x0F, 0x11, 0x54, 0x24, 0x40, /* 18 movups [rsp+0x20..], xmm0-5 */
    0x0F, 0x11, 0x5C, 0x24, 0x50, 0x0F, 0x11, 0x64, 0x24, 0x60, 0x0F, 0x11, 0x6C, 0x24, 0x70,
    0x48, 0x8D, 0x8C, 0x24, 0xC0, 0x00, 0x00, 0x00,                               /*  48 lea rcx, [rsp+0xc0] */
    0x48, 0x89, 0xEA, 0x45, 0x89, 0xF0, 0x49, 0x89, 0xF1,                         /*  56 mov rdx, rbp; mov r8d, r14d; mov r9, rsi */
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xD0,                               /*  65 mov rax, ZoneGroupsCaught (imm64 at 67); call rax */
    0x0F, 0x10, 0x44, 0x24, 0x20, 0x0F, 0x10, 0x4C, 0x24, 0x30, 0x0F, 0x10, 0x54, 0x24, 0x40, /* 77 movups xmm0-5, [rsp+0x20..] */
    0x0F, 0x10, 0x5C, 0x24, 0x50, 0x0F, 0x10, 0x64, 0x24, 0x60, 0x0F, 0x10, 0x6C, 0x24, 0x70,
    0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00,                                     /* 107 add rsp, 0x88 */
    0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58,             /* 114 pop r11-r8, rdx, rcx, rax */
    0x48, 0x8B, 0x7C, 0x24, 0x68,                                                 /* 125 mov rdi, [rsp+0x68] */
    0xE9, 0, 0, 0, 0};                                                            /* 130 jmp 0xF4C350 (rel32 at 131) */

static void ZoneGroupsWrite(LONG n, const ZoneCatch* c)
{
    wchar_t name[32];
    swprintf(name, 32, L"zone-groups-%ld.tsv", n);
    HANDLE file = OpenLog(name);
    char line[256];
    DWORD written;
    int shown = c->groups < DIF_ZONE_CATCH_GROUPS ? (int)c->groups : DIF_ZONE_CATCH_GROUPS, flat = 0, largest = -1;
    float low = 3.4e38f, high = -3.4e38f;
    if (file != INVALID_HANDLE_VALUE)
    {
        int length = snprintf(line, sizeof line, "level\t%u\troute cells\t%u\tzone cells\t%u\tgroups\t%u\ngroup\ttype\tcells\tx\ty\tmin\tmax\theight\n",
            c->difficulty, c->cells, c->zoneCells, c->groups);
        WriteFile(file, line, (DWORD)length, &written, NULL);
    }
    for (int g = 0; g < shown; g++)
    {
        const ZoneGroupCaught* z = &c->group[g];
        if (z->min == 0.0f && z->max == 0.0f && z->height == 0.0f) flat++;
        if (z->height < low) low = z->height;
        if (z->height > high) high = z->height;
        if (largest < 0 || z->cells > c->group[largest].cells) largest = g;
        if (file == INVALID_HANDLE_VALUE) continue;
        int length = snprintf(line, sizeof line, "%d\t%d\t%u\t%.0f\t%.0f\t%g\t%g\t%g\n", g, z->type, z->cells, z->x, z->y, z->min, z->max, z->height);
        WriteFile(file, line, (DWORD)length, &written, NULL);
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (largest < 0) { Note("difficulty: zone groups (level %u): none over %u route cells", c->difficulty, c->cells); return; }
    const ZoneGroupCaught* big = &c->group[largest];
    Note("difficulty: zone groups (level %u): %u over %u zone cells of %u route cells, %d of the first %d at 0 with no height sampled, "
        "heights %g..%g; the largest %u cells of type %d at (%.0f, %.0f): min %g, max %g, height %g (zone-groups-%ld.tsv)",
        c->difficulty, c->groups, c->zoneCells, c->cells, flat, shown, low, high, big->cells, big->type, big->x, big->y, big->min,
        big->max, big->height, n);
}

/* Called by kZoneGroupsStub on the generating thread: frame is force_flatten_zones' rsp (+0x68 the markers, 8 bytes a
 * cell: visited, then the group at +4; +0x70 the route cells; +0x78 their info, 0x20 a cell: the type at +0xC, the
 * cell at +0x18, its x and y at +0), rbp+0x60 the group table (12 bytes a group: min, max, height), context+0x18 the
 * level. */
static void ZoneGroupsCaught(uintptr_t frame, uintptr_t rbp, uint32_t groups, uintptr_t context)
{
    uint64_t markers = 0, info = 0, table = 0, level = 0;
    uint32_t cells = 0;
    uint8_t difficulty = 0;
    if (SafeRead(frame + 0x68, &markers, 8) || SafeRead(frame + 0x70, &cells, 4) || SafeRead(frame + 0x78, &info, 8) ||
        SafeRead(rbp + 0x60, &table, 8) || SafeRead(context + 0x18, &level, 8) || SafeRead(level + 0xF9, &difficulty, 1))
        return;
    ZoneCatch c;
    memset(&c, 0, sizeof c);
    c.difficulty = difficulty;
    c.cells = cells;
    c.groups = groups;
    for (uint32_t g = 0; g < groups && g < DIF_ZONE_CATCH_GROUPS; g++)
    {
        float entry[3] = {0};
        SafeRead(table + 12 * (uint64_t)g, entry, 12);
        c.group[g].type = -1;
        c.group[g].min = entry[0]; c.group[g].max = entry[1]; c.group[g].height = entry[2];
    }
    for (uint32_t i = 0; i < cells && i < 8192; i++)
    {
        uint8_t marker[8], entry[0x20];
        if (SafeRead(markers + 8 * (uint64_t)i, marker, 8) || SafeRead(info + 0x20 * (uint64_t)i, entry, sizeof entry)) break;
        uint32_t group;
        memcpy(&group, marker + 4, 4);
        if (marker[0] == 0) continue;
        c.zoneCells++;
        if (group >= groups || group >= DIF_ZONE_CATCH_GROUPS) continue;
        ZoneGroupCaught* z = &c.group[group];
        uint64_t cell = 0;
        float position[2] = {0};
        memcpy(&z->type, entry + 0xC, 4); memcpy(&cell, entry + 0x18, 8);
        SafeRead(cell, position, 8);
        z->x += position[0]; z->y += position[1]; z->cells++;
    }
    for (int g = 0; g < DIF_ZONE_CATCH_GROUPS; g++)
        if (c.group[g].cells) { c.group[g].x /= (float)c.group[g].cells; c.group[g].y /= (float)c.group[g].cells; }
    if (c.zoneCells == 0) return;
    LONG n = InterlockedIncrement(&g_zoneCatches);
    if (n > 16) return;
    g_zoneCatch = c;
    ZoneGroupsWrite(n, &c);
}

/* A type 0/1 map's towns are its zone cells: SelectSubRegionCellsFromHeightGroup (0xF37500) gives each route cell a
 * subregion by a noise value sampled at the cell (0xF3A2E0), in world metres. A larger map keeps level 10's route
 * cells by spacing them out (65.75 m at 2 x, 40 m at level 10), so each noise blob covered a third of the cells and
 * FinalizeVoronoi's 100 m zone cells (64 m) fewer again: a town came out as scattered single cells, each flattened to
 * its own height (research d11-sparse-cities §8; zone-groups-<n>.tsv: 16 zone cells in 11 groups, 10 of one cell). On a
 * larger map the noise is sampled at 1024 + (p - 1024) x 40 / the spacing, so a town keeps level 10's cells, each
 * larger: one flattened area. Off with HELLFORGE_D11_TOWN_NOISE=0, for comparing. */
#define DIF_TOWN_NOISE_SITE 0xF37BA9u /* movss xmm2, [rdi+4]; lea rcx, [rbp-0x70]; movss xmm1, [rdi] (13), then the call */
#define DIF_TOWN_NOISE_CALL 0xF37BB6u
static const uint8_t kTownNoisePin[13] = {0xF3, 0x0F, 0x10, 0x57, 0x04, 0x48, 0x8D, 0x4D, 0x90, 0xF3, 0x0F, 0x10, 0x0F};
static const uint8_t kTownNoiseStub[76] = {
    0x48, 0x8B, 0x44, 0x24, 0x50,                                     /*  0 mov rax, [rsp+0x50] (the pass context) */
    0x48, 0x8B, 0x40, 0x08,                                           /*  5 mov rax, [rax+8] (lg+0x2D20) */
    0xF3, 0x0F, 0x10, 0x0F, 0xF3, 0x0F, 0x10, 0x57, 0x04,             /*  9 movss xmm1, [rdi]; movss xmm2, [rdi+4] (the cell) */
    0x81, 0x78, 0x5C, 0, 0, 0, 0,                                     /* 18 cmp dword [rax+0x5c], disk / 1024 (at 21) */
    0x75, 0x28,                                                       /* 25 jne 67 */
    0xF3, 0x0F, 0x10, 0x05, 0x2D, 0x00, 0x00, 0x00,                   /* 27 movss xmm0, [rip+0x2d] (1024, at 80) */
    0xF3, 0x0F, 0x10, 0x1D, 0x29, 0x00, 0x00, 0x00,                   /* 35 movss xmm3, [rip+0x29] (40 / the spacing, at 84) */
    0xF3, 0x0F, 0x5C, 0xC8, 0xF3, 0x0F, 0x5C, 0xD0,                   /* 43 subss xmm1, xmm0; subss xmm2, xmm0 */
    0xF3, 0x0F, 0x59, 0xCB, 0xF3, 0x0F, 0x59, 0xD3,                   /* 51 mulss xmm1, xmm3; mulss xmm2, xmm3 */
    0xF3, 0x0F, 0x58, 0xC8, 0xF3, 0x0F, 0x58, 0xD0,                   /* 59 addss xmm1, xmm0; addss xmm2, xmm0 */
    0x48, 0x8D, 0x4D, 0x90,                                           /* 67 lea rcx, [rbp-0x70] */
    0xE9, 0, 0, 0, 0};                                                /* 71 jmp 0xF37BB6 (rel32 at 72) */

/* FinalizeVoronoi type 1 (0xF68050) refills each route cell with points at its type's size (+0x18C; HellForge scales
 * that read, kFinalizeSpacingStub, so zone cells are 100 m), then reads the size twice more, unscaled: each zone point
 * clears the other points within 2 x size (0xF686FC) and proposes a ring of four at x, y +- size (0xF688C3), kept where
 * no point sits exactly. At level 10 the ring of a town's inner cells lands on the next zone point (64 m), so only its
 * edge gets one and the zone cells touch: one group, one plateau. With a 100 m lattice and a 64 m ring every pair of
 * zone cells got two other cells between them (+64, +36 m), so each was flattened alone, the terrain between left as
 * it was (research d11-sparse-cities §9). On a larger map both reads are scaled as the refill's (x the cell factor,
 * rounded up to a quarter metre: 64 -> 100). Off with HELLFORGE_D11_TOWN_RING=0, for comparing. */
static const struct { uint32_t site; uint8_t pin[8]; uint8_t rip; uint8_t round; uint32_t back; } kTownRingSites[2] = {
    {0xF686FCu, {0xF3, 0x0F, 0x10, 0x91, 0x8C, 0x01, 0x00, 0x00}, 0x15, 0xD2, 0xF68704u},  /* the clear: movss xmm2, [rcx+0x18c] */
    {0xF688C3u, {0xF3, 0x0F, 0x10, 0xA9, 0x8C, 0x01, 0x00, 0x00}, 0x2D, 0xED, 0xF688CBu}}; /* the ring: movss xmm5, [rcx+0x18c] */
/* the replaced movss; the level ([rsp+0x70] the pass, +0x18); cmp its disk; jne back; x factor x 4, ceil, x 0.25; back */
static const uint8_t kTownRingStub[56] = {
    0, 0, 0, 0, 0, 0, 0, 0,                                           /*  0 movss xmmN, [rcx+0x18c] (the pin) */
    0x48, 0x8B, 0x44, 0x24, 0x70,                                     /*  8 mov rax, [rsp+0x70] */
    0x48, 0x8B, 0x40, 0x18,                                           /* 13 mov rax, [rax+0x18] */
    0x81, 0xB8, 0x7C, 0x2D, 0x00, 0x00, 0, 0, 0, 0,                   /* 17 cmp dword [rax+0x2d7c], disk / 1024 (at 23) */
    0x75, 0x16,                                                       /* 27 jne 51 */
    0xF3, 0x0F, 0x59, 0, 0, 0, 0, 0,                                  /* 29 mulss xmmN, [rip+factor x 4] (modrm at 32, rel32 at 33) */
    0x66, 0x0F, 0x3A, 0x0A, 0, 0x02,                                  /* 37 roundss xmmN, xmmN, ceil (modrm at 41) */
    0xF3, 0x0F, 0x59, 0, 0, 0, 0, 0,                                  /* 43 mulss xmmN, [rip+0.25] (modrm at 46, rel32 at 47) */
    0xE9, 0, 0, 0, 0};                                                /* 51 jmp back (rel32 at 52) */

/* A zone town's outer wall is its edge rules (types 29-31, rules 0-6, 32 x 16 m, rule +0x24 & 0x3C) in the subregion
 * stamps (op 0x2B): 0xF31BD0 keeps a slot's area edges that border another type (the town's ring) within a reach of
 * 25 m of the slot (0xF337DB), and the rule places only on its side code (0xF33839). At level 10 an area is 32 m, its
 * slot 16 m from the edge; a larger map's areas are 50 m with 2 x 2 slots, so few slots reached an edge and the towns
 * lost most of their walls (research d11-sparse-cities §10). On a larger map the reach is 25 m x the cell factor (39 m
 * at 2 x); the walls stay 32 m pieces. Counted: edge rules tried, no edge in reach (0xF33802), the wrong side. Off with
 * HELLFORGE_D11_TOWN_WALLS=0, for comparing (the counters stay). */
#define DIF_TOWN_REACH_SITE 0xF337DBu /* movss xmm0, [rip+25.0] (8), the edge test's reach argument */
#define DIF_TOWN_REACH_BACK 0xF337E3u
static const uint8_t kTownReachPin[8] = {0xF3, 0x0F, 0x10, 0x05, 0x31, 0x3E, 0x49, 0x01};
/* The counted branches: je to the next rule (0F 84 rel32), {site, its target, counter}. */
static const struct { uint32_t site; uint8_t pin[6]; uint32_t target; uint8_t counter; } kTownEdgeRefusals[2] = {
    {0xF33802u, {0x0F, 0x84, 0x44, 0x13, 0x00, 0x00}, 0xF34B4Cu, 5},  /* no edge of another type in reach */
    {0xF33839u, {0x0F, 0x84, 0x0D, 0x13, 0x00, 0x00}, 0xF34B4Cu, 6}}; /* the edge on the rule's wrong side */

static int TownWallsWanted(void)
{
    wchar_t value[8];
    DWORD length = GetEnvironmentVariableW(L"HELLFORGE_D11_TOWN_WALLS", value, 8);
    return !(length == 1 && value[0] == L'0');
}

static int TownRingWanted(void)
{
    wchar_t value[8];
    DWORD length = GetEnvironmentVariableW(L"HELLFORGE_D11_TOWN_RING", value, 8);
    return !(length == 1 && value[0] == L'0');
}

static int TownNoiseWanted(void)
{
    wchar_t value[8];
    DWORD length = GetEnvironmentVariableW(L"HELLFORGE_D11_TOWN_NOISE", value, 8);
    return !(length == 1 && value[0] == L'0');
}

static int TownBorderWanted(void)
{
    wchar_t value[8];
    DWORD length = GetEnvironmentVariableW(L"HELLFORGE_D11_TOWN_BORDER", value, 8);
    return !(length == 1 && value[0] == L'0');
}

/* NULL when every town site is the reviewed one. */
static const char* TownPinsFailed(uintptr_t game)
{
    if (!LargerMaps()) return NULL;
    uint8_t zone[8];
    for (int i = 0; i < 5; i++)
        if (SafeRead(game + kTownBorderSites[i].site, zone, kTownBorderSites[i].length) || memcmp(zone, kTownBorderSites[i].pin, kTownBorderSites[i].length) != 0)
            return "the town placers are not the reviewed ones (a map_scale above 1 needs them)";
    for (int i = 0; i < 2; i++)
        if (SafeRead(game + kTownRefusals[i].site, zone, 6) || memcmp(zone, kTownRefusals[i].pin, 6) != 0)
            return "the town placers are not the reviewed ones (a map_scale above 1 needs them)";
    if (SafeRead(game + DIF_TOWN_PLACED, zone, 5) || memcmp(zone, kTownPlacedPin, 5) != 0 ||
        SafeRead(game + DIF_TOWN_MERGE, zone, 7) || memcmp(zone, kTownMergePin, 7) != 0)
        return "the town placers are not the reviewed ones (a map_scale above 1 needs them)";
    if (SafeRead(game + DIF_RECORD_STORE, zone, 7) || memcmp(zone, kRecordStorePin, 7) != 0)
        return "the destruction records' registration is not the reviewed one (a map_scale above 1 needs it)";
    if (SafeRead(game + DIF_ZONE_GROUPS_SITE, zone, 5) || memcmp(zone, kZoneGroupsPin, 5) != 0)
        return "the height zones' grouping is not the reviewed one (a map_scale above 1 needs it)";
    uint8_t noise[13];
    if (SafeRead(game + DIF_TOWN_NOISE_SITE, noise, 13) || memcmp(noise, kTownNoisePin, 13) != 0)
        return "the subregion noise is not the reviewed one (a map_scale above 1 needs it)";
    for (int i = 0; i < 2; i++)
        if (SafeRead(game + kTownRingSites[i].site, zone, 8) || memcmp(zone, kTownRingSites[i].pin, 8) != 0)
            return "the town cells' ring is not the reviewed one (a map_scale above 1 needs it)";
    if (SafeRead(game + DIF_TOWN_REACH_SITE, zone, 8) || memcmp(zone, kTownReachPin, 8) != 0)
        return "the town walls' edge test is not the reviewed one (a map_scale above 1 needs it)";
    for (int i = 0; i < 2; i++)
        if (SafeRead(game + kTownEdgeRefusals[i].site, zone, 6) || memcmp(zone, kTownEdgeRefusals[i].pin, 6) != 0)
            return "the town walls' edge test is not the reviewed one (a map_scale above 1 needs it)";
    return NULL;
}

static uint8_t* TownRel32(uint8_t* p, uintptr_t target) { int32_t rel = (int32_t)(int64_t)(target - (uintptr_t)(p + 4)); memcpy(p, &rel, 4); return p + 4; }
static uint8_t* TownCount(uint8_t* p, int counter) /* lock inc dword [rip -> counter] */
{
    *p++ = 0xF0; *p++ = 0xFF; *p++ = 0x05;
    return TownRel32(p, (uintptr_t)&g_townCounters[counter]);
}
static uint8_t* TownJump(uint8_t* p, uintptr_t target) { *p++ = 0xE9; return TownRel32(p, target); }

/* The stubs on one page, the counters on the next (writable), then each site sent to its stub. */
static const char* InstallTowns(DifficultyInstall* d)
{
    if (!LargerMaps()) return NULL;
    uintptr_t game = d->game;
    uint8_t* code = AllocateNearGame(game, 0x2000);
    if (code == NULL) return "no memory within 2 GB of game.dll for the towns";
    g_townCounters = (volatile LONG*)(code + 0x1000);
    int border = TownBorderWanted();
    float diskBits = d->disk / 1024.0f;
    uintptr_t stubs[11];
    uint8_t* p = code;
    for (int i = 0; i < 5; i++) /* cmp [base+0x5c], disk; jne vanilla; the border's mulss; jmp back; R's mulss; jmp back */
    {
        uint8_t length = kTownBorderSites[i].length;
        stubs[i] = (uintptr_t)p;
        memcpy(p, kTownBorderSites[i].cmp, 3); p += 3;
        memcpy(p, &diskBits, 4); p += 4;
        *p++ = 0x75; *p++ = (uint8_t)(length + 5);
        memcpy(p, kTownBorderSites[i].pin, length); p[length - 1] = 0x54; p += length;
        p = TownJump(p, game + kTownBorderSites[i].back);
        memcpy(p, kTownBorderSites[i].pin, length); p += length;
        p = TownJump(p, game + kTownBorderSites[i].back);
    }
    for (int i = 0; i < 2; i++) /* jae counted; jmp on; counted: lock inc; jmp the refusal */
    {
        stubs[5 + i] = (uintptr_t)p;
        *p++ = 0x0F; *p++ = 0x83; uint32_t five = 5; memcpy(p, &five, 4); p += 4;
        p = TownJump(p, game + kTownRefusals[i].site + 6);
        p = TownCount(p, kTownRefusals[i].counter);
        p = TownJump(p, game + kTownRefusals[i].target);
    }
    stubs[7] = (uintptr_t)p; /* call the placer; count it; on */
    *p++ = 0xE8; p = TownRel32(p, game + DIF_TOWN_PLACED_CALL);
    p = TownCount(p, 0);
    p = TownJump(p, game + DIF_TOWN_PLACED + 5);
    stubs[8] = (uintptr_t)p; /* mov rax, [rsp+0xa0] (the pass record + 8); cmp r13d, [rax+0x278] (its capacity); jb room;
                              * count; jmp the drop; room: the two instructions; jmp on */
    static const uint8_t merge[] = {0x48, 0x8B, 0x84, 0x24, 0xA0, 0x00, 0x00, 0x00, 0x44, 0x3B, 0xA8, 0x78, 0x02, 0x00, 0x00, 0x72, 0x0C};
    memcpy(p, merge, sizeof merge); p += sizeof merge;
    p = TownCount(p, 3);
    p = TownJump(p, game + DIF_TOWN_MERGE_DROP);
    memcpy(p, kTownMergePin, 7); p += 7;
    p = TownJump(p, game + DIF_TOWN_MERGE + 7);
    stubs[9] = (uintptr_t)p; /* test r8, r8; jz skip; the two stores; jmp on; skip: count; jmp on */
    *p++ = 0x4D; *p++ = 0x85; *p++ = 0xC0; *p++ = 0x74; *p++ = 0x0C;
    memcpy(p, kRecordStorePin, 7); p += 7;
    p = TownJump(p, game + DIF_RECORD_STORE + 7);
    p = TownCount(p, 4);
    p = TownJump(p, game + DIF_RECORD_STORE + 7);
    stubs[10] = (uintptr_t)p; /* the zone groups' catch */
    memcpy(p, kZoneGroupsStub, sizeof kZoneGroupsStub);
    uint64_t caught = (uint64_t)(uintptr_t)ZoneGroupsCaught;
    memcpy(p + 67, &caught, 8);
    TownRel32(p + 131, game + DIF_ZONE_GROUPS_SITE + 5);
    p += sizeof kZoneGroupsStub;
    p = (uint8_t*)(((uintptr_t)p + 15) & ~(uintptr_t)15);
    uint8_t* noiseStub = p; /* the subregion noise at level 10's spacing: the stub, then 1024 and 40 / the spacing */
    float spacing = d->spacingValue != NULL && *d->spacingValue > 0.0f ? *d->spacingValue : 40.0f, noiseScale[2] = {1024.0f, 40.0f / spacing};
    memcpy(p, kTownNoiseStub, sizeof kTownNoiseStub);
    memcpy(p + 21, &diskBits, 4);
    TownRel32(p + 72, game + DIF_TOWN_NOISE_CALL);
    memcpy(p + 80, noiseScale, 8);
    p += 88;
    uint8_t* ringStubs[2];
    for (int i = 0; i < 2; i++) /* the ring and its clear at the refill's size (d->cellFactor: x 4, then 0.25) */
    {
        ringStubs[i] = p;
        memcpy(p, kTownRingStub, sizeof kTownRingStub);
        memcpy(p, kTownRingSites[i].pin, 8);
        memcpy(p + 23, &diskBits, 4);
        p[32] = kTownRingSites[i].rip; p[41] = kTownRingSites[i].round; p[46] = kTownRingSites[i].rip;
        if (d->cellFactor != NULL)
        {
            TownRel32(p + 33, (uintptr_t)&d->cellFactor[0]);
            TownRel32(p + 47, (uintptr_t)&d->cellFactor[1]);
        }
        TownRel32(p + 52, game + kTownRingSites[i].back);
        p += 64;
    }
    /* the walls' reach: mov r11, [rbp] (the pass); mov r11, [r11+8] (lg+0x2D20); xmm0 = 25; on a larger map's disk xmm0 =
     * the reach, counted; back. Its constants after it, then the two counted refusals (je counted; jmp on; counted:
     * lock inc; jmp the next rule). */
    int walls = TownWallsWanted();
    uint8_t* reachStub = p;
    static const uint8_t reach[] = {0x4C, 0x8B, 0x5D, 0x00, 0x4D, 0x8B, 0x5B, 0x08, 0xF3, 0x0F, 0x10, 0x05, 0, 0, 0, 0,
        0x41, 0x81, 0x7B, 0x5C, 0, 0, 0, 0, 0x75, 0x0F, 0xF3, 0x0F, 0x10, 0x05, 0, 0, 0, 0};
    memcpy(p, reach, sizeof reach);
    float reachValues[2] = {25.0f, walls && d->cellFactor != NULL ? 25.0f * d->cellFactor[0] * d->cellFactor[1] : 25.0f};
    memcpy(p + 48, reachValues, 8);
    TownRel32(p + 12, (uintptr_t)(p + 48));
    memcpy(p + 20, &diskBits, 4);
    TownRel32(p + 30, (uintptr_t)(p + 52));
    TownCount(p + 34, 7);
    TownJump(p + 41, game + DIF_TOWN_REACH_BACK);
    p += 64;
    uintptr_t edgeStubs[2];
    for (int i = 0; i < 2; i++)
    {
        edgeStubs[i] = (uintptr_t)p;
        *p++ = 0x0F; *p++ = kTownEdgeRefusals[i].pin[1]; uint32_t five = 5; memcpy(p, &five, 4); p += 4;
        p = TownJump(p, game + kTownEdgeRefusals[i].site + 6);
        p = TownCount(p, kTownEdgeRefusals[i].counter);
        p = TownJump(p, game + kTownEdgeRefusals[i].target);
    }
    DWORD protect;
    if (p - code > 0x1000 || !VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ, &protect)) return "the towns' stubs could not be made executable";
    FlushInstructionCache(GetCurrentProcess(), code, 0x1000);
    const struct { uint32_t site; uint8_t length; } sites[11] = {
        {kTownBorderSites[0].site, 6}, {kTownBorderSites[1].site, 6}, {kTownBorderSites[2].site, 6}, {kTownBorderSites[3].site, 5},
        {kTownBorderSites[4].site, 5}, {kTownRefusals[0].site, 6}, {kTownRefusals[1].site, 6}, {DIF_TOWN_PLACED, 5},
        {DIF_TOWN_MERGE, 7}, {DIF_RECORD_STORE, 7}, {DIF_ZONE_GROUPS_SITE, 5}};
    for (int i = border ? 0 : 5; i < 11; i++) /* the border's five only when wanted */
    {
        uint8_t jump[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
        int32_t rel = (int32_t)(int64_t)(stubs[i] - (game + sites[i].site + 5));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + sites[i].site, jump, sites[i].length)) return "the towns could not be patched";
    }
    const struct { uint32_t site; uint8_t length; uintptr_t stub; } edgeSites[3] = {
        {DIF_TOWN_REACH_SITE, 8, (uintptr_t)reachStub}, {kTownEdgeRefusals[0].site, 6, edgeStubs[0]}, {kTownEdgeRefusals[1].site, 6, edgeStubs[1]}};
    for (int i = 0; i < 3; i++)
    {
        uint8_t jump[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
        int32_t rel = (int32_t)(int64_t)(edgeSites[i].stub - (game + edgeSites[i].site + 5));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + edgeSites[i].site, jump, edgeSites[i].length)) return "the towns could not be patched";
    }
    int ring = TownRingWanted() && d->cellFactor != NULL;
    for (int i = 0; ring && i < 2; i++)
    {
        uint8_t jump[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
        int32_t rel = (int32_t)(int64_t)((uintptr_t)ringStubs[i] - (game + kTownRingSites[i].site + 5));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + kTownRingSites[i].site, jump, 8)) return "the towns could not be patched";
    }
    int noise = TownNoiseWanted();
    if (noise)
    {
        uint8_t jump[13] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        int32_t rel = (int32_t)(int64_t)((uintptr_t)noiseStub - (game + DIF_TOWN_NOISE_SITE + 5));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + DIF_TOWN_NOISE_SITE, jump, 13)) return "the towns could not be patched";
    }
    Note("difficulty: towns on larger maps %s (HELLFORGE_D11_TOWN_BORDER=0 keeps level 10's radius)",
        border ? "are built up to the border" : "keep level 10's radius");
    if (noise) Note("difficulty: towns on larger maps keep level 10's cells (their noise at x %.4f; HELLFORGE_D11_TOWN_NOISE=0 keeps it in metres)", noiseScale[1]);
    else Note("difficulty: towns on larger maps keep their noise in metres (HELLFORGE_D11_TOWN_NOISE=0)");
    Note("difficulty: town cells on larger maps %s (HELLFORGE_D11_TOWN_RING=0 keeps level 10's)",
        ring ? "keep their ring at the refill's spacing, so they touch" : "keep level 10's ring spacing");
    Note("difficulty: town walls on larger maps reach their edge within %.1f m (HELLFORGE_D11_TOWN_WALLS=0 keeps 25 m)", reachValues[1]);
    return NULL;
}
