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

#define DIF_TOWN_COUNTERS 5 /* placed, refused past R, refused locations full, merge dropped, records skipped */
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
 * zone-groups-<n>.tsv and a line, for the first 8 generations at any level (a level-10 one compares). */
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
    LONG n = InterlockedIncrement(&g_zoneCatches);
    if (n > 8) return;
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
    g_zoneCatch = c;
    ZoneGroupsWrite(n, &c);
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
    Note("difficulty: towns on larger maps %s (HELLFORGE_D11_TOWN_BORDER=0 keeps level 10's radius)",
        border ? "are built up to the border" : "keep level 10's radius");
    return NULL;
}
