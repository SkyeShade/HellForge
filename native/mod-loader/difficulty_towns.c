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
    uintptr_t stubs[10];
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
    DWORD protect;
    if (p - code > 0x1000 || !VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ, &protect)) return "the towns' stubs could not be made executable";
    FlushInstructionCache(GetCurrentProcess(), code, 0x1000);
    const struct { uint32_t site; uint8_t length; } sites[10] = {
        {kTownBorderSites[0].site, 6}, {kTownBorderSites[1].site, 6}, {kTownBorderSites[2].site, 6}, {kTownBorderSites[3].site, 5},
        {kTownBorderSites[4].site, 5}, {kTownRefusals[0].site, 6}, {kTownRefusals[1].site, 6}, {DIF_TOWN_PLACED, 5},
        {DIF_TOWN_MERGE, 7}, {DIF_RECORD_STORE, 7}};
    for (int i = border ? 0 : 5; i < 10; i++) /* the border's five only when wanted */
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
