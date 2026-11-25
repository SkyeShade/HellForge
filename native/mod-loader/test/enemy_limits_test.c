/* The enemy limits (enemy_limits.c): rows by difficulty level holding the game's values, checked writes, the code left
 * alone until a row differs, then each site pointed at its stub, and the stubs run against stand-ins for the code they
 * return to: the patrol gate, the roaming gate, the despawn pass's start and switch, its "any enemy" count, the far
 * distance and the spawn gate's immediate, all following the mission's level. On a fake game.dll image. Included by
 * stats_test.c. */

typedef uint32_t (*LimitsProbe)(void* context, uint32_t value);

/* A thunk that loads the stub's registers (prefix) from the probe's arguments, then jumps to the site. */
static LimitsProbe LimitsFixtureThunk(uint8_t* at, const uint8_t* prefix, size_t size, uintptr_t site)
{
    memcpy(at, prefix, size);
    at[size] = 0x49; at[size + 1] = 0xBB; memcpy(at + size + 2, &site, 8); /* mov r11, site */
    at[size + 10] = 0x41; at[size + 11] = 0xFF; at[size + 12] = 0xE3;     /* jmp r11 */
    return (LimitsProbe)(void*)at;
}

/* The rel32 target of the instruction at code whose rel32 sits at code + at and which ends at code + end. */
static uintptr_t LimitsFixtureTarget(const uint8_t* code, size_t at, size_t end)
{
    int32_t rel;
    memcpy(&rel, code + at, 4);
    return (uintptr_t)code + end + rel;
}

static void LimitsFixtureWrites(void)
{
    uint32_t v = 1001;
    float f = 5;
    assert(strstr(DirectorRefuses("EnemyLimits", (uintptr_t)&g_limitsRows[7], 0, &v, 4), "0 to 1000"));
    v = 1000; assert(DirectorRefuses("EnemyLimits", (uintptr_t)&g_limitsRows[7], 8, &v, 4) == NULL);
    v = 2; assert(strstr(DirectorRefuses("EnemyLimits", (uintptr_t)&g_limitsRows[7], 12, &v, 4), "0 (off) or 1"));
    memcpy(&v, &f, 4); assert(strstr(DirectorRefuses("EnemyLimits", (uintptr_t)&g_limitsRows[7], 20, &v, 4), "10 to 2000"));
    f = 300; memcpy(&v, &f, 4); assert(DirectorRefuses("EnemyLimits", (uintptr_t)&g_limitsRows[7], 20, &v, 4) == NULL);
    assert(DirectorStride("EnemyLimits") == EL_ROW && DirectorKind("EnemyLimits"));
}

/* The stand-ins the stubs jump back to: 1 for a spawn (or a pass run), 0 for a refusal; the "any" count in eax. */
static void LimitsFixtureReturns(uint8_t* game)
{
    static const uint8_t one[] = {0xB8, 1, 0, 0, 0, 0xC3}, zero[] = {0xB8, 0, 0, 0, 0, 0xC3};
    static const uint8_t oneRdi[] = {0xB8, 1, 0, 0, 0, 0x5F, 0xC3}, zeroRdi[] = {0xB8, 0, 0, 0, 0, 0x5F, 0xC3};
    static const uint8_t ebx[] = {0x89, 0xD8, 0x5B, 0xC3}; /* mov eax, ebx; pop rbx; ret */
    memcpy(game + EL_PATROL_SPAWN, one, sizeof one); memcpy(game + EL_PATROL_BACK, zero, sizeof zero);
    memcpy(game + EL_ROAMING_BACK, one, sizeof one); memcpy(game + EL_ROAMING_REFUSE, zero, sizeof zero);
    memcpy(game + EL_PASS_BACK, oneRdi, sizeof oneRdi); memcpy(game + EL_PASS_SKIP, zeroRdi, sizeof zeroRdi);
    memcpy(game + EL_ANY_BACK, ebx, sizeof ebx);
}

/* Each site jumps to its stub; the stubs read the block and return where the game's code did. */
static void LimitsFixtureSites(uint8_t* game)
{
    const uint8_t* code = g_limitsCode;
    const EnemyLimitsLive* live = (const EnemyLimitsLive*)(code + 0x1000);
    static const struct { uint32_t site, size, stub; } sites[] = {
        {EL_PATROL_SITE, 8, 0}, {EL_ROAMING_SITE, 8, 0x40}, {EL_PASS_SITE, 17, 0x80}, {EL_ANY_SITE, 11, 0xC0}};
    for (size_t i = 0; i < sizeof sites / sizeof sites[0]; i++)
    {
        assert(game[sites[i].site] == 0xE9 && LimitsFixtureTarget(game + sites[i].site, 1, 5) == (uintptr_t)code + sites[i].stub);
        for (uint32_t b = 5; b < sites[i].size; b++) assert(game[sites[i].site + b] == 0xCC);
    }
    assert(LimitsFixtureTarget(game + EL_FAR_READ, 0, 4) == (uintptr_t)&live->farSquared);
    assert(LimitsFixtureTarget(code, 5, 9) == (uintptr_t)&live->patrolGate);   /* cmp eax, [rip+...] */
    assert(LimitsFixtureTarget(code, 0x82, 0x87) == (uintptr_t)&live->despawnOff); /* cmp byte [rip+...], 0 */
    assert(LimitsFixtureTarget(code, 0xC4, 0xC8) == (uintptr_t)game + EL_POINT_SIX);
}

/* The four stubs run, through thunks that load the registers the game's code had there. */
static void LimitsFixtureRun(uint8_t* game, uint8_t* thunks, uint8_t* world)
{
    static const uint8_t patrol[] = {0x89, 0xD0}, roaming[] = {0x48, 0x89, 0xC8, 0x89, 0xD1};
    static const uint8_t pass[] = {0x57, 0x48, 0x89, 0xCF}, any[] = {0x53, 0x89, 0xD3};
    LimitsProbe patrols = LimitsFixtureThunk(thunks, patrol, sizeof patrol, (uintptr_t)game + EL_PATROL_SITE);
    LimitsProbe roams = LimitsFixtureThunk(thunks + 0x40, roaming, sizeof roaming, (uintptr_t)game + EL_ROAMING_SITE);
    LimitsProbe passes = LimitsFixtureThunk(thunks + 0x80, pass, sizeof pass, (uintptr_t)game + EL_PASS_SITE);
    LimitsProbe anys = LimitsFixtureThunk(thunks + 0xC0, any, sizeof any, (uintptr_t)game + EL_ANY_SITE);
    uint8_t counts[0x20] = {0};
    uint32_t living = 150, despawning = 5, entries = 160;
    memcpy(counts + 0x1C, &living, 4);
    memcpy(world + 0x75E9D4, &despawning, 4);
    memcpy(world + 0x75E9E4, &entries, 4);
    /* level 7: patrols to 200, roaming to 300, the despawn pass off */
    assert(patrols(counts, 50) == 1 && patrols(counts, 51) == 0);   /* 150 + 50 <= 200 */
    assert(roams(counts, 149) == 1 && roams(counts, 150) == 0);     /* 150 + 149 < 300 */
    assert(passes(world, 0) == 0);
    assert(anys(NULL, 5) == 5 + 260);                               /* the start 250, ten more */
    /* level 3: the game's */
    uint8_t* mission = *(uint8_t**)(*(uint8_t**)(game + EL_HIVEMIND) + 0x650);
    mission[0xF9] = 3;
    EnemyLimitsTick();
    assert(patrols(counts, 0) == 0);                                /* 150 > 100 */
    living = 40; memcpy(counts + 0x1C, &living, 4);
    assert(patrols(counts, 60) == 1 && patrols(counts, 61) == 0);   /* 40 + 60 <= 100 */
    assert(roams(counts, 29) == 1 && roams(counts, 30) == 0);       /* under 70 */
    assert(passes(world, 0) == 1);                                  /* 160 >= 5 + 100 */
    entries = 104; memcpy(world + 0x75E9E4, &entries, 4);
    assert(passes(world, 0) == 0);
    assert(anys(NULL, 5) == 5 + 110);
    assert(((const EnemyLimitsLive*)(g_limitsCode + 0x1000))->patrols == 2); /* the two let through, kept across levels */
}

static void EnemyLimitsFixture(void)
{
    uint8_t* game = (uint8_t*)VirtualAlloc(NULL, DIR_IMAGE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    uint8_t* thunks = (uint8_t*)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    uint8_t* world = (uint8_t*)calloc(1, 0x75F000);
    uint8_t* manager = (uint8_t*)calloc(1, 0x700);
    uint8_t* mission = (uint8_t*)calloc(1, 0x100);
    assert(game != NULL && thunks != NULL && world != NULL && manager != NULL && mission != NULL);
    g_dirGame = game;
    g_directorImage = DirFakeImage;
    g_limitsState = 0; g_limitsFirst = 0;
    int ready = 1;

    /* no rows while the code is not the reviewed one (it may still be decoded), then every level the game's values */
    assert(DirectorRow("EnemyLimits", 7, &ready) == 0 && !ready && g_limitsState == 0);
    for (size_t i = 0; i < EL_PINS; i++) memcpy(game + kLimitsPins[i].rva, kLimitsPins[i].bytes, kLimitsPins[i].length);
    EnemyLimitsRow* row = (EnemyLimitsRow*)DirectorRow("EnemyLimits", 7, &ready);
    assert(row == &g_limitsRows[7] && ready && g_limitsState == 1 && row->patrolGate == 100 && row->roamingGate == 70 &&
        row->spawnGate == 448 && row->despawn == 1 && row->despawnStart == 100 && row->despawnDistance == 150.0f);
    assert(DirectorRow("EnemyLimits", 0, &ready) == 0 && DirectorRow("EnemyLimits", 12, &ready) == 0 && ready);
    LimitsFixtureWrites();

    /* the game's values everywhere: the code is left as it is */
    uintptr_t at = (uintptr_t)manager, level = (uintptr_t)mission;
    memcpy(game + EL_HIVEMIND, &at, 8); memcpy(manager + 0x650, &level, 8); mission[0xF9] = 7;
    EnemyLimitsTick();
    assert(g_limitsState == 1 && !memcmp(game + kLimitsPins[0].rva, kLimitsPins[0].bytes, kLimitsPins[0].length));

    /* level 7 changed live, as the editor does: the sites go to the stubs, the block takes the level's values */
    HfMod* mod = &g_mods[0];
    HfField gate = HF_FIELD(EnemyLimits, patrol_gate), roam = HF_FIELD(EnemyLimits, roaming_gate), units = HF_FIELD(EnemyLimits, spawn_gate);
    HfField pass = HF_FIELD(EnemyLimits, despawn), start = HF_FIELD(EnemyLimits, despawn_start), distance = HF_FIELD(EnemyLimits, despawn_distance);
    HfValue two = HF_U32(200), three = HF_U32(300), six = HF_U32(600), off = HF_U32(0), late = HF_U32(250), wide = HF_F32(300), lots = HF_U32(5000);
    assert(ApiSetLive(mod, 7, &gate, &two, HF_LIVE_SHARED) != 0 && ApiSetLive(mod, 7, &roam, &three, HF_LIVE_SHARED) != 0);
    assert(ApiSetLive(mod, 7, &units, &six, HF_LIVE_SHARED) != 0 && ApiSetLive(mod, 7, &pass, &off, HF_LIVE_SHARED) != 0);
    assert(ApiSetLive(mod, 7, &start, &late, HF_LIVE_SHARED) != 0 && ApiSetLive(mod, 7, &distance, &wide, HF_LIVE_SHARED) != 0);
    assert(ApiSetLive(mod, 7, &gate, &lots, HF_LIVE_SHARED) == 0 && row->patrolGate == 200);
    EnemyLimitsTick();
    assert(g_limitsState == 2 && g_limitsCode != NULL);
    const EnemyLimitsLive* live = (const EnemyLimitsLive*)(g_limitsCode + 0x1000);
    uint32_t imm;
    memcpy(&imm, game + EL_UNITS_IMM, 4);
    assert(live->patrolGate == 200 && live->roamingGate == 300 && live->despawnOff == 1 && live->despawnStart == 250 &&
        live->despawnAny == 260 && live->farSquared == 90000.0f && imm == 600);
    LimitsFixtureSites(game);
    LimitsFixtureReturns(game);
    LimitsFixtureRun(game, thunks, world); /* ends at level 3 */
    memcpy(&imm, game + EL_UNITS_IMM, 4);
    assert(imm == 448 && live->farSquared == 22500.0f);

    /* no mission: the game's values */
    mission[0xF9] = 7;
    memset(manager + 0x650, 0, 8);
    EnemyLimitsTick();
    assert(live->patrolGate == 100 && live->despawnOff == 0);

    g_editCount = 0;
    g_directorImage = DirectorGame;
    g_limitsState = 0; g_limitsCode = NULL; g_limitsShownLevel = -1;
    memset(g_limitsRows, 0, sizeof g_limitsRows);
    free(mission); free(manager); free(world);
    VirtualFree(thunks, 0, MEM_RELEASE);
    VirtualFree(game, 0, MEM_RELEASE);
}
