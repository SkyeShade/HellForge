/* The spawn director's tables (director.c): roster rows by id with their weights where level 11 left them, compositions
 * by roster, family and row, every write checked (a squad within the queue's 32, ranges in order, finite weights), live
 * and from the boot stats pass; and the difficulty rows without level 11: the game's own copy, each write copied to every
 * copy that held the same bytes. On a fake game.dll image. Included by stats_test.c. */

static uint8_t* g_dirGame;
static uintptr_t DirFakeImage(size_t* size) { if (size) *size = DIR_IMAGE; return (uintptr_t)g_dirGame; }

static uint32_t g_deathFired[4];
static int g_deathCalls;
static void DeathFired(void* context, uint32_t type, uint32_t node, uint8_t r9)
{
    (void)context; (void)node; (void)r9;
    g_deathFired[g_deathCalls++ & 3] = type;
}

static void DeathExplosionTest(void)
{
    int ready = 0;
    const DeathSite* hulk = NULL;
    for (size_t i = 0; i < DTH_SITES; i++) if (kDeathSites[i].type == 40) { hulk = &kDeathSites[i]; break; }
    assert(hulk != NULL && DTH_SITES > 20);
    uint32_t* row = (uint32_t*)DirectorRow("EnemyDeathExplosion", hulk->id, &ready);
    assert(row != NULL && ready && *row == 40 && DirectorRow("EnemyDeathExplosion", DTH_SITES + 1, &ready) == 0 && ready);
    assert(DirectorStride("EnemyDeathExplosion") == DTH_ROW);
    uint32_t v = 125;
    assert(DirectorRefuses("EnemyDeathExplosion", (uintptr_t)row, 0, &v, 4) == NULL && DirectorRefuses("EnemyDeathExplosion", (uintptr_t)row, 4, &v, 4) != NULL);
    /* the behaviour's context: +0 the entity's descriptor, its resource first */
    uint64_t descriptor[3] = {hulk->resource, 0, 0}, other[3] = {0x1234567890ABCDEFull, 0, 0};
    void* context = descriptor;
    void* stranger = other;
    g_deathOriginal = DeathFired;
    g_deathCalls = 0;
    *row = 125;
    DeathExplosionDetour(&context, 40, 0x446C84DCu, 1);   /* the Hulk's own: swapped */
    DeathExplosionDetour(&context, 262, 0x9B115563u, 0);  /* another type it fires: as it was */
    DeathExplosionDetour(&stranger, 40, 0x446C84DCu, 1);  /* another enemy firing 40: as it was */
    assert(g_deathCalls == 3 && g_deathFired[0] == 125 && g_deathFired[1] == 262 && g_deathFired[2] == 40);
    *row = 0;
    DeathExplosionDetour(&context, 40, 0x446C84DCu, 1);   /* none: nothing fires */
    assert(g_deathCalls == 3);
    /* live: None through set_live, and back to the game's */
    HfField death = HF_FIELD(EnemyDeathExplosion, explosion);
    HfValue none = HF_U32(0), own = HF_U32(40);
    *row = 40;
    assert(ApiSetLive(&g_mods[0], hulk->id, &death, &none, HF_LIVE_SHARED) != 0 && *row == 0);
    assert(ApiSetLive(&g_mods[0], hulk->id, &death, &own, HF_LIVE_SHARED) != 0 && *row == 40);
    g_deathOriginal = NULL;
}

static void DirectorFixture(void)
{
    g_dirGame = (uint8_t*)VirtualAlloc(NULL, DIR_IMAGE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(g_dirGame != NULL);
    uintptr_t game = (uintptr_t)g_dirGame;
    uint8_t* rows[5];
    for (int r = 0; r < 5; r++)
    {
        rows[r] = (uint8_t*)calloc(kRosters[r].rows, DIR_ROSTER_ROW);
        uint64_t at = (uint64_t)(uintptr_t)rows[r], count = kRosters[r].rows;
        memcpy(g_dirGame + kRosters[r].descriptor, &at, 8);
        memcpy(g_dirGame + kRosters[r].descriptor + 8, &count, 8);
    }
    /* roster 0's family 1 (the second): two compositions; group 0 draws from a roster group, group 1 from none */
    uint8_t* compositions = (uint8_t*)calloc(2, DIR_COMPOSITION_ROW);
    uint64_t at = (uint64_t)(uintptr_t)compositions, two = 2;
    memcpy(g_dirGame + kRosters[0].descriptor + 0x20 + 0x10 * 1, &at, 8);
    memcpy(g_dirGame + kRosters[0].descriptor + 0x28 + 0x10 * 1, &two, 8);
    uint8_t* second = compositions + DIR_COMPOSITION_ROW;
    uint32_t key = 0xCF576442u, seven = 7, one = 1, ten = 10, four = 4;
    memcpy(second + 0, &key, 4); memcpy(second + 4, &seven, 4);
    memcpy(second + 0xEC, &one, 4); memcpy(second + 0xF0, &ten, 4); memcpy(second + 0xF4, &one, 4); memcpy(second + 0xF8, &four, 4);
    g_dirGame[kWeightReaders[0][0] + kWeightReaders[0][1]] = 0x30;
    for (size_t c = 0; c < sizeof kDifficultyCopies / sizeof kDifficultyCopies[0]; c++)
        for (uint32_t level = 1; level <= DIF_VANILLA; level++)
            memcpy(g_dirGame + kDifficultyCopies[c] + (level - 1) * DIF_ROW + 4, &level, 4);
    g_directorImage = DirFakeImage;
    int added = g_difficultyAdded, state = g_difficultyState;
    g_difficultyAdded = 0; g_difficultyState = 1;
    int ready;

    /* rows by id: the weights at +0x30, no level 11 yet; a composition by roster, family and row */
    assert(DirectorRow("EnemyRoster", (2u << 16) | 3, &ready) == (uintptr_t)rows[2] + 3 * DIR_ROSTER_ROW + 0x30 && ready);
    assert(DirectorRow("EnemyRosterLevel11", 3, &ready) == 0 && ready);
    assert(DirectorRow("EnemyRoster", (2u << 16) | 61, &ready) == 0 && DirectorRow("EnemyRoster", 5u << 16, &ready) == 0);
    assert(DirectorRow("EnemyComposition", (0u << 16) | (1u << 12) | 1, &ready) == (uintptr_t)second);
    assert(DirectorRow("EnemyComposition", (0u << 16) | (1u << 12) | 2, &ready) == 0 && DirectorRow("EnemyComposition", 2u << 12, &ready) == 0);
    /* a roster whose count is not the reviewed one: none of its rows */
    uint64_t wrong = 44;
    memcpy(g_dirGame + kRosters[0].descriptor + 8, &wrong, 8);
    assert(DirectorRow("EnemyRoster", 1, &ready) == 0 && ready);
    wrong = kRosters[0].rows; memcpy(g_dirGame + kRosters[0].descriptor + 8, &wrong, 8);
    /* level 11 added: the weights at +0x2C, its own at +0x54; tech levels up to 11 */
    g_dirGame[kWeightReaders[0][0] + kWeightReaders[0][1]] = 0x2C;
    assert(DirectorRow("EnemyRoster", 3, &ready) == (uintptr_t)rows[0] + 3 * DIR_ROSTER_ROW + 0x2C);
    assert(DirectorRow("EnemyRosterLevel11", 3, &ready) == (uintptr_t)rows[0] + 3 * DIR_ROSTER_ROW + 0x54);
    g_difficultyAdded = 1; g_difficultyState = 0; /* its move is still to come: wait */
    assert(DirectorRow("EnemyRoster", 3, &ready) == 0 && !ready);
    g_difficultyAdded = 0; g_difficultyState = 1;

    /* the checks: a squad within 32, a group that names enemies, ranges in order, weights finite */
    uint32_t v = 32, total = 26;
    assert(DirectorRefuses("EnemyComposition", (uintptr_t)second, 4, &v, 4) == NULL);
    v = 33; assert(strstr(DirectorRefuses("EnemyComposition", (uintptr_t)second, 4, &v, 4), "32"));
    memcpy(second + 12, &key, 4); memcpy(second + 16, &total, 4); /* group 1: 26 of the 32 */
    v = 7; assert(strstr(DirectorRefuses("EnemyComposition", (uintptr_t)second, 4, &v, 4), "32"));
    v = 6; assert(DirectorRefuses("EnemyComposition", (uintptr_t)second, 4, &v, 4) == NULL);
    v = 1; assert(strstr(DirectorRefuses("EnemyComposition", (uintptr_t)second, 28, &v, 4), "no enemies")); /* group 2 has no key */
    v = 11; assert(DirectorRefuses("EnemyComposition", (uintptr_t)second, 0xF0, &v, 4) == NULL); /* level 11 is there */
    g_dirGame[kWeightReaders[0][0] + kWeightReaders[0][1]] = 0x30;
    assert(DirectorRefuses("EnemyComposition", (uintptr_t)second, 0xF0, &v, 4) != NULL); /* ... and now it is not */
    v = 0; assert(strstr(DirectorRefuses("EnemyComposition", (uintptr_t)second, 0xF0, &v, 4), "1 to 10"));
    v = 4; assert(DirectorRefuses("EnemyComposition", (uintptr_t)second, 0xEC, &v, 4) == NULL);
    v = 11; assert(strstr(DirectorRefuses("EnemyComposition", (uintptr_t)second, 0xEC, &v, 4), "1 to 10"));
    v = 3; memcpy(second + 0xF0, &v, 4); v = 4;
    assert(strstr(DirectorRefuses("EnemyComposition", (uintptr_t)second, 0xEC, &v, 4), "exceed"));
    memcpy(second + 0xF0, &ten, 4);
    float f = -1;
    assert(DirectorRefuses("EnemyRoster", 0, 0, &f, 4) != NULL && DirectorRefuses("EnemyComposition", (uintptr_t)second, 0xE0, &f, 4) != NULL);
    f = 0.001f; assert(strstr(DirectorRefuses("EnemyComposition", (uintptr_t)second, 0xE4, &f, 4), "0.01"));
    f = 2.5f; assert(DirectorRefuses("EnemyRoster", 0, 0, &f, 4) == NULL);
    /* a group's key: a family its own roster holds (roster 0's), or 0 once its count is */
    uint32_t automatonKey = 0x1A0EC706u, noKey = 0;
    assert(strstr(DirectorRefuses("EnemyComposition", (uintptr_t)second, 0, &key, 4), "no such enemy family"));
    memcpy(rows[0] + 5 * DIR_ROSTER_ROW, &key, 4);
    memcpy(rows[2] + 7 * DIR_ROSTER_ROW, &automatonKey, 4);
    assert(DirectorRefuses("EnemyComposition", (uintptr_t)second, 0, &key, 4) == NULL);
    assert(DirectorRefuses("EnemyComposition", (uintptr_t)second, 24, &key, 4) == NULL); /* an empty group takes one */
    assert(strstr(DirectorRefuses("EnemyComposition", (uintptr_t)second, 0, &automatonKey, 4), "no such enemy family")); /* another faction's */
    assert(strstr(DirectorRefuses("EnemyComposition", (uintptr_t)second, 0, &noKey, 4), "count to 0"));
    assert(DirectorRefuses("EnemyComposition", (uintptr_t)second, 24, &noKey, 4) == NULL);
    assert(DirectorRefuses("EnemyComposition", 0x1000, 0, &key, 4) != NULL); /* no roster's composition */

    /* a family's last weight at a level stays (roster 0's Scavengers: tier 1 at levels 1-3, tier 2 at 4-10) */
    float one_f = 1, zero = 0;
    uint8_t* scav = rows[0];
    for (int r = 0; r < 4; r++) memcpy(scav + r * DIR_ROSTER_ROW, &key, 4);
    for (int l = 0; l < 3; l++) memcpy(scav + 0x30 + 4 * l, &one_f, 4);
    for (int l = 3; l < 10; l++) memcpy(scav + DIR_ROSTER_ROW + 0x30 + 4 * l, &one_f, 4);
    uintptr_t tier1 = (uintptr_t)scav + 0x30, tier2 = (uintptr_t)scav + DIR_ROSTER_ROW + 0x30;
    assert(DirectorRow("EnemyRoster", 1, &ready) == tier2);
    assert(strstr(DirectorRefuses("EnemyRoster", tier2, 36, &zero, 4), "last spawn weight")); /* the reported crash */
    assert(strstr(DirectorRefuses("EnemyRoster", tier1, 0, &zero, 4), "last spawn weight"));
    assert(DirectorRefuses("EnemyRoster", tier1, 36, &zero, 4) == NULL);  /* already 0 there: nothing changes */
    assert(DirectorRefuses("EnemyRoster", tier2, 36, &one_f, 4) == NULL); /* a weight above 0 is always allowed */
    /* a third row of the family, weighted at level 10 but only on planets with its tag: tier 2 stays the untagged one */
    uint32_t tag = 0xDF4CB4E5u, tagB = 0xFD8B9706u, tagC = 0x33FA1AB8u;
    uint8_t* third = scav + 2 * DIR_ROSTER_ROW;
    uint8_t* tier2row = scav + DIR_ROSTER_ROW;
    memcpy(third + 0x58, &tag, 4);
    memcpy(third + 0x30 + 36, &one_f, 4);
    assert(strstr(DirectorRefuses("EnemyRoster", tier2, 36, &zero, 4), "planet tag"));
    assert(DirectorRefuses("EnemyRoster", tier2 + DIR_ROSTER_ROW, 36, &zero, 4) == NULL); /* the tagged one may go */
    memcpy(third + 0x58, &zero, 4);
    memcpy(third + 0x68 + 8, &tag, 4); /* after an empty exclude slot: the game reads no further, so it draws everywhere */
    assert(DirectorRefuses("EnemyRoster", tier2, 36, &zero, 4) == NULL);
    memcpy(third + 0x68, &tag, 4); /* a tag that rules it out is a tag too */
    assert(strstr(DirectorRefuses("EnemyRoster", tier2, 36, &zero, 4), "planet tag"));
    memcpy(scav + 3 * DIR_ROSTER_ROW + 0x30 + 36, &one_f, 4); /* a fourth, untagged: tier 2 may go */
    assert(DirectorRefuses("EnemyRoster", tier2, 36, &zero, 4) == NULL);
    uint32_t otherKey = 0x5BD8103u; /* another family's weight does not count */
    memcpy(scav + 3 * DIR_ROSTER_ROW, &otherKey, 4);
    assert(strstr(DirectorRefuses("EnemyRoster", tier2, 36, &zero, 4), "planet tag"));
    /* tagged rows judged tag by tag (Illuminate rows need different subfactions): tier 2 needs tag or tagB */
    memset(third + 0x58, 0, 0x20);
    memcpy(tier2row + 0x58, &tag, 4);
    memcpy(tier2row + 0x5C, &tagB, 4);
    memcpy(third + 0x58, &tag, 4);
    assert(strstr(DirectorRefuses("EnemyRoster", tier2, 36, &zero, 4), "planet tag")); /* nothing left on tagB planets */
    memcpy(third + 0x60, &tagB, 4);
    assert(DirectorRefuses("EnemyRoster", tier2, 36, &zero, 4) == NULL);
    memcpy(third + 0x68, &tagC, 4); /* the third rules out tagC planets, which tier 2 draws on */
    assert(strstr(DirectorRefuses("EnemyRoster", tier2, 36, &zero, 4), "planet tag"));
    memcpy(tier2row + 0x68, &tagC, 4); /* tier 2 rules them out too */
    assert(DirectorRefuses("EnemyRoster", tier2, 36, &zero, 4) == NULL);
    memset(third + 0x60, 0, 4);
    memcpy(tier2row + 0x6C, &tagB, 4); /* a need it rules out itself draws nowhere */
    assert(DirectorRefuses("EnemyRoster", tier2, 36, &zero, 4) == NULL);
    memset(tier2row + 0x58, 0, 0x20);
    memset(third + 0x58, 0, 0x20);
    /* level 11 added: the weights at +0x2C, level 11's at +0x54, both judged */
    for (int r = 0; r < 4; r++) memset(scav + r * DIR_ROSTER_ROW + 0x2C, 0, 0x2C);
    for (int l = 3; l < 11; l++) memcpy(scav + DIR_ROSTER_ROW + 0x2C + 4 * l, &one_f, 4);
    g_dirGame[kWeightReaders[0][0] + kWeightReaders[0][1]] = 0x2C;
    tier2 = (uintptr_t)scav + DIR_ROSTER_ROW + 0x2C;
    assert(strstr(DirectorRefuses("EnemyRoster", tier2, 36, &zero, 4), "last spawn weight"));
    assert(strstr(DirectorRefuses("EnemyRosterLevel11", tier2 + 0x28, 0, &zero, 4), "last spawn weight"));
    memcpy(scav + 3 * DIR_ROSTER_ROW, &key, 4);
    memcpy(scav + 3 * DIR_ROSTER_ROW + 0x54, &one_f, 4);
    assert(DirectorRefuses("EnemyRosterLevel11", tier2 + 0x28, 0, &zero, 4) == NULL);
    assert(strstr(DirectorRefuses("EnemyRoster", tier2, 36, &zero, 4), "last spawn weight")); /* level 10 still its only */
    assert(DirectorRefuses("EnemyRoster", 0x1000, 36, &zero, 4) == NULL); /* not a roster row: nothing to judge */
    g_dirGame[kWeightReaders[0][0] + kWeightReaders[0][1]] = 0x30;
    for (int r = 0; r < 4; r++) memset(scav + r * DIR_ROSTER_ROW, 0, DIR_ROSTER_ROW);
    for (int l = 3; l < 10; l++) memcpy(scav + DIR_ROSTER_ROW + 0x30 + 4 * l, &one_f, 4);
    memcpy(scav + DIR_ROSTER_ROW, &key, 4); /* for the live and boot checks below: tier 2 alone */

    /* demolition templates: waiting for the destruction system, then a row by index once all eleven read as reviewed;
     * "none" and "Count" stay, and only a force of 1 to 255 is written */
    uint8_t* system = (uint8_t*)calloc(1, 0x40);
    uint8_t* templates = (uint8_t*)calloc(DIR_DEMOLITION_ROWS, DIR_DEMOLITION_ROW);
    for (uint32_t i = 0; i < DIR_DEMOLITION_ROWS; i++)
    {
        memcpy(templates + i * DIR_DEMOLITION_ROW, &i, 4);
        memcpy(templates + i * DIR_DEMOLITION_ROW + 8, kDemolitionTail[i], 8);
    }
    assert(DirectorRow("DemolitionTemplate", 3, &ready) == 0 && !ready);
    uint64_t pointer = (uint64_t)(uintptr_t)system;
    memcpy(g_dirGame + DIR_DESTRUCTION, &pointer, 8);
    assert(DirectorRow("DemolitionTemplate", 3, &ready) == 0 && !ready);
    pointer = (uint64_t)(uintptr_t)templates;
    memcpy(system + 0x30, &pointer, 8);
    assert(DirectorRow("DemolitionTemplate", 3, &ready) == (uintptr_t)templates + 3 * DIR_DEMOLITION_ROW && ready);
    assert(DirectorRow("DemolitionTemplate", 0, &ready) == 0 && DirectorRow("DemolitionTemplate", 10, &ready) == 0 && ready);
    templates[7 * DIR_DEMOLITION_ROW + 12] = 0; /* not as reviewed: nothing */
    assert(DirectorRow("DemolitionTemplate", 3, &ready) == 0 && ready);
    templates[7 * DIR_DEMOLITION_ROW + 12] = 1;
    v = 45; assert(DirectorRefuses("DemolitionTemplate", (uintptr_t)templates + 3 * DIR_DEMOLITION_ROW, 4, &v, 4) == NULL);
    v = 256; assert(strstr(DirectorRefuses("DemolitionTemplate", (uintptr_t)templates + 3 * DIR_DEMOLITION_ROW, 4, &v, 4), "255"));
    v = 0; assert(DirectorRefuses("DemolitionTemplate", (uintptr_t)templates + 3 * DIR_DEMOLITION_ROW, 4, &v, 4) != NULL);
    v = 45; assert(DirectorRefuses("DemolitionTemplate", (uintptr_t)templates + 3 * DIR_DEMOLITION_ROW, 8, &v, 4) != NULL);

    /* summons: no rows while the abilities' code is not the reviewed one (it may still be decoded), then the code's own
     * values; a family of a primary roster and 0 to 16 of them, put into the code by the update */
    assert(DirectorRow("EnemySummon", 463, &ready) == 0 && !ready);
    for (size_t i = 0; i < SUM_SITES; i++)
    {
        const SummonSite* site = &kSummons[i];
        uint32_t stub = 0x1150000u + (uint32_t)i * 0x10, family = i ? 0x8F7704F8u : 0xAFF5B26Bu, many = i ? 1 : 3;
        int32_t rel = (int32_t)(site->function - (stub + 8));
        memcpy(g_dirGame + SUM_ABILITIES + (site->ability - 1) * 4, &stub, 4);
        static const uint8_t caseStub[] = {0x41, 0x8B, 0xD0, 0xE8}; /* mov edx, r8d; call */
        memcpy(g_dirGame + stub, caseStub, 4); memcpy(g_dirGame + stub + 4, &rel, 4);
        memcpy(g_dirGame + site->function, site->prologue, sizeof site->prologue);
        memcpy(g_dirGame + site->key, site->keyOp, site->opLength); memcpy(g_dirGame + site->key + site->opLength, &family, 4);
        memcpy(g_dirGame + site->count, site->countOp, site->opLength); memcpy(g_dirGame + site->count + site->opLength, &many, 4);
        rel = (int32_t)(SUM_SPAWN - (site->call + 5));
        g_dirGame[site->call] = 0xE8; memcpy(g_dirGame + site->call + 1, &rel, 4);
    }
    memcpy(g_dirGame + SUM_SPAWN, kSummonSpawnPin, sizeof kSummonSpawnPin);
    uint8_t* warriors = (uint8_t*)DirectorRow("EnemySummon", 463, &ready);
    assert(warriors == (uint8_t*)&g_summonRows[1] && ready && g_summonRows[1].family == 0x8F7704F8u && g_summonRows[1].count == 1);
    assert(DirectorRow("EnemySummon", 333, &ready) == (uintptr_t)&g_summonRows[0] && g_summonRows[0].count == 3);
    assert(DirectorRow("EnemySummon", 1702, &ready) == 0 && ready);
    v = 17; assert(strstr(DirectorRefuses("EnemySummon", (uintptr_t)warriors, 4, &v, 4), "0 to 16"));
    v = 16; assert(DirectorRefuses("EnemySummon", (uintptr_t)warriors, 4, &v, 4) == NULL);
    assert(DirectorRefuses("EnemySummon", (uintptr_t)warriors, 0, &key, 4) == NULL);
    assert(DirectorRefuses("EnemySummon", (uintptr_t)warriors, 0, &automatonKey, 4) == NULL); /* any faction's roster */
    v = 0x12345678u; assert(strstr(DirectorRefuses("EnemySummon", (uintptr_t)warriors, 0, &v, 4), "no faction"));
    assert(DirectorRefuses("EnemySummon", (uintptr_t)warriors, 0, &noKey, 4) != NULL);
    assert(DirectorStride("EnemySummon") == SUM_ROW);
    g_summonRows[1].family = key; g_summonRows[1].count = 4;
    SummonsTick();
    memcpy(&v, g_dirGame + kSummons[1].key + 3, 4); assert(v == key);
    memcpy(&v, g_dirGame + kSummons[1].count + 3, 4); assert(v == 4);
    memcpy(&v, g_dirGame + kSummons[0].count + 4, 4); assert(v == 3); /* the other left as it was */
    g_summonRows[1].family = 0x8F7704F8u; g_summonRows[1].count = 1;
    SummonsTick();
    memcpy(&v, g_dirGame + kSummons[1].key + 3, 4); assert(v == 0x8F7704F8u);
    HfField summoned = HF_FIELD(EnemySummon, count);
    HfValue six = HF_U32(6), lots = HF_U32(40);
    assert(ApiSetLive(&g_mods[0], 333, &summoned, &six, HF_LIVE_SHARED) != 0 && g_summonRows[0].count == 6);
    assert(ApiSetLive(&g_mods[0], 333, &summoned, &lots, HF_LIVE_SHARED) == 0 && g_summonRows[0].count == 6);
    memset(rows[0] + 5 * DIR_ROSTER_ROW, 0, 4);

    /* death explosions: a row per enemy and explosion, the game's type at first; the hook swaps that enemy's only, 0 none */
    DeathExplosionTest();

    /* live: the editor's writes go through the same checks */
    HfMod* mod = &g_mods[0];
    HfField count = HF_FIELD(EnemyComposition, group_0_count), weight = HF_FIELD_AT(EnemyRoster, weights, 4);
    HfValue fourty = HF_U32(40), five = HF_U32(5), half = HF_F32(0.5f);
    HfRef composition = (0u << 16) | (1u << 12) | 1;
    assert(ApiSetLive(mod, composition, &count, &fourty, HF_LIVE_SHARED) == 0);
    memcpy(&v, second + 4, 4); assert(v == 7);
    assert(ApiSetLive(mod, composition, &count, &five, HF_LIVE_SHARED) != 0);
    memcpy(&v, second + 4, 4); assert(v == 5);
    assert(ApiSetLive(mod, (1u << 16) | 2, &weight, &half, HF_LIVE_SHARED) != 0);
    memcpy(&f, rows[1] + 2 * DIR_ROSTER_ROW + 0x30 + 16, 4); assert(f == 0.5f);
    HfField tenth = HF_FIELD_AT(EnemyRoster, weights, 9);
    HfValue none = HF_F32(0);
    assert(ApiSetLive(mod, 1, &tenth, &none, HF_LIVE_SHARED) == 0); /* the Scavengers' last weight at level 10 */
    memcpy(&f, rows[0] + DIR_ROSTER_ROW + 0x30 + 36, 4); assert(f == 1);
    HfField force = HF_FIELD(DemolitionTemplate, force);
    HfValue strong = HF_U32(45), crushing = HF_U32(300);
    assert(ApiSetLive(mod, 3, &force, &strong, HF_LIVE_SHARED) != 0);
    memcpy(&v, templates + 3 * DIR_DEMOLITION_ROW + 4, 4); assert(v == 45);
    assert(ApiSetLive(mod, 3, &force, &crushing, HF_LIVE_SHARED) == 0 && ApiSetLive(mod, 10, &force, &strong, HF_LIVE_SHARED) == 0);

    /* the game's own difficulty table: level 3's encounter points in its copy and in every copy that held the same */
    float budget = 100, other = 999;
    for (size_t c = 0; c < sizeof kDifficultyCopies / sizeof kDifficultyCopies[0]; c++)
        memcpy(g_dirGame + kDifficultyCopies[c] + 2 * DIF_ROW + 0xE4, c == 7 ? &other : &budget, 4);
    assert(TableRowOf("DifficultySettings", 3, &ready) == game + kDifficultyCopies[DIR_DIFFICULTY_COPY] + 2 * DIF_ROW);
    assert(TableRowOf("DifficultySettings", 11, &ready) == 0 && ready);
    HfField encounter = HF_FIELD(DifficultySettings, encounter_points);
    HfValue more = HF_F32(250);
    assert(ApiSetLive(mod, 3, &encounter, &more, HF_LIVE_SHARED) != 0);
    for (size_t c = 0; c < sizeof kDifficultyCopies / sizeof kDifficultyCopies[0]; c++)
    {
        memcpy(&f, g_dirGame + kDifficultyCopies[c] + 2 * DIF_ROW + 0xE4, 4);
        assert(f == (c == 7 ? 999.0f : 250.0f)); /* a copy that differed is left as it was */
    }

    /* the boot pass: a stats file's director and difficulty lines, checked as live ones are */
    int saved = g_statCount;
    StatEdit e = {0};
    e.mod = mod; e.shared = 1; e.unverified = 1; e.size = 4;
    StatEdit squad = e, roster = e, level = e, empty = e;
    strcpy(squad.kind, "EnemyComposition"); squad.record = (int64_t)composition; squad.offset = 4; squad.value = 99; strcpy(squad.label, "squad");
    strcpy(roster.kind, "EnemyRoster"); roster.record = (2 << 16) | 5; roster.offset = 8; roster.floating = 1; strcpy(roster.label, "roster");
    f = 3; memcpy(&roster.value, &f, 4);
    strcpy(level.kind, "DifficultySettings"); level.record = 5; level.offset = 0xE8; level.floating = 1; strcpy(level.label, "level 5");
    f = 640; memcpy(&level.value, &f, 4);
    strcpy(empty.kind, "EnemyRoster"); empty.record = 1; empty.offset = 36; empty.floating = 1; empty.value = 0; strcpy(empty.label, "empty");
    g_stats[g_statCount++] = squad; g_stats[g_statCount++] = roster; g_stats[g_statCount++] = level; g_stats[g_statCount++] = empty;
    g_directorDone = 0;
    StatsDirectorReady();
    assert(g_directorDone && !g_stats[saved].applied && g_stats[saved + 1].applied && g_stats[saved + 2].applied);
    assert(!g_stats[saved + 3].applied); /* a stats file's line that empties a family is refused too */
    memcpy(&f, rows[0] + DIR_ROSTER_ROW + 0x30 + 36, 4); assert(f == 1);
    memcpy(&f, rows[2] + 5 * DIR_ROSTER_ROW + 0x30 + 8, 4); assert(f == 3);
    memcpy(&f, g_dirGame + kDifficultyCopies[0] + 4 * DIF_ROW + 0xE8, 4); assert(f == 640); /* copied to the others */
    g_statCount = saved;
    /* a weight moved within a family: its 0 waits for the file's other lines, unless a later line writes its field */
    uint8_t* moved = rows[0] + 2 * DIR_ROSTER_ROW;
    memcpy(moved, &key, 4);
    StatEdit lower = empty, raise = empty, again = empty;
    raise.record = 2; memcpy(&raise.value, &one_f, 4); strcpy(raise.label, "raise");
    f = 0.5f; memcpy(&again.value, &f, 4); strcpy(again.label, "again");
    g_stats[g_statCount++] = lower; g_stats[g_statCount++] = raise;
    g_directorDone = 0;
    StatsDirectorReady();
    assert(g_stats[saved].applied && g_stats[saved + 1].applied);
    memcpy(&f, rows[0] + DIR_ROSTER_ROW + 0x30 + 36, 4); assert(f == 0);
    memcpy(&f, moved + 0x30 + 36, 4); assert(f == 1);
    g_statCount = saved;
    memcpy(rows[0] + DIR_ROSTER_ROW + 0x30 + 36, &one_f, 4);
    memset(moved + 0x30, 0, 0x28);
    g_stats[g_statCount++] = lower; g_stats[g_statCount++] = raise; g_stats[g_statCount++] = again;
    g_directorDone = 0;
    StatsDirectorReady();
    memcpy(&f, rows[0] + DIR_ROSTER_ROW + 0x30 + 36, 4); assert(f == 0.5f); /* load order: the later line's value */
    memcpy(&f, moved + 0x30 + 36, 4); assert(f == 1);
    g_statCount = saved;
    memset(moved, 0, DIR_ROSTER_ROW);
    memcpy(rows[0] + DIR_ROSTER_ROW + 0x30 + 36, &one_f, 4);

    g_editCount = 0;
    g_directorImage = DirectorGame;
    g_difficultyAdded = added; g_difficultyState = state;
    for (int r = 0; r < 5; r++) free(rows[r]);
    free(compositions);
    VirtualFree(g_dirGame, 0, MEM_RELEASE);
}
