/* Enemies mods add to the faction rosters (enemy_rosters.c) on a fake game.dll image: the refusals, the grown copies
 * (the game's rows first, a mod's row a scaled copy of its like's, rules after the game's), entries without an entity or
 * a row left out, the budget's weight through the stub, edits held back until the rosters grew, a mod taken out.
 * Included by stats_test.c after director_test.c. */

#define ERT_LIKE 0xAAAull
#define ERT_UNIT 0x111ull
#define ERT_BUG_UNIT 0x222ull
#define ERT_SWAPPED 0x333ull
#define ERT_SHARED 0x777ull

static int ErtKnown(uint64_t entity) { return entity == ERT_LIKE || entity == ERT_UNIT || entity == ERT_BUG_UNIT || entity == ERT_SWAPPED; }

/* A roster row of `unit`: weights 1 to 10 at levels 1 to 10, a cap and a budget weight. */
static void ErtRow(uint8_t* row, uint64_t unit, float budget)
{
    uint32_t key = 0x5BD8103u, cap = 20;
    memcpy(row, &key, 4);
    memcpy(row + 8, &unit, 8);
    memcpy(row + 0x18, &cap, 4);
    memcpy(row + 0x24, &budget, 4);
    for (int l = 0; l < 10; l++) { float w = (float)(l + 1); memcpy(row + 0x30 + 4 * l, &w, 4); }
}

static void ErtRefusals(HfMod* mod)
{
    HfEnemy e = {0};
    assert(ApiAddEnemy(mod, &e) == 1); /* no size */
    e.size = sizeof e; e.unit = ERT_UNIT; e.like = ERT_LIKE;
    assert(ApiAddEnemy(mod, &e) == 1); /* no name */
    e.name = "tester/enemies/refused";
    e.factions = 8; assert(ApiAddEnemy(mod, &e) == 1);
    e.factions = 0; e.lowest_level = 11; assert(ApiAddEnemy(mod, &e) == 1);
    e.lowest_level = 5; e.highest_level = 4; assert(ApiAddEnemy(mod, &e) == 1);
    e.highest_level = 0; e.weight = -1; assert(ApiAddEnemy(mod, &e) == 1);
    e.weight = 0; e.like = 0; assert(ApiAddEnemy(mod, &e) == 1);
    HfEnemySwap s = {sizeof s};
    s.name = "tester/enemies/refused_rule"; s.unit = ERT_UNIT; s.replacement = ERT_UNIT;
    assert(ApiAddEnemySwap(mod, &s) == 1); /* itself */
    s.replacement = ERT_SWAPPED; s.highest_level = 101; assert(ApiAddEnemySwap(mod, &s) == 1);
    s.highest_level = 0; s.priority = 1001; assert(ApiAddEnemySwap(mod, &s) == 1);
    assert(g_erEnemyCount == 0 && g_erSwapCount == 0 && !EnemyRostersPending());
}

static void ErtAdd(HfMod* mod)
{
    HfEnemy e = {sizeof e};
    e.name = "tester/enemies/alpha"; e.unit = ERT_UNIT; e.like = ERT_LIKE; e.weight = 0.5f; e.lowest_level = 4;
    assert(ApiAddEnemy(mod, &e) == 0);
    assert(ApiAddEnemy(mod, &e) == 1); /* its name is taken */
    e.name = "tester/enemies/no_entity"; e.unit = 0x999; e.lowest_level = 0;
    assert(ApiAddEnemy(mod, &e) == 0);
    e.name = "tester/enemies/no_row"; e.unit = ERT_UNIT; e.like = 0xDEAD;
    assert(ApiAddEnemy(mod, &e) == 0);
    e.name = "tester/enemies/bug_only"; e.unit = ERT_BUG_UNIT; e.like = ERT_LIKE; e.weight = 0; e.factions = HF_ENEMY_TERMINIDS;
    assert(ApiAddEnemy(mod, &e) == 0);
    HfEnemySwap s = {sizeof s};
    s.name = "tester/enemies/assault"; s.unit = ERT_UNIT; s.replacement = ERT_SWAPPED; s.tag = "GM_BotAssault"; s.priority = 5;
    assert(ApiAddEnemySwap(mod, &s) == 0);
    s.name = "tester/enemies/no_replacement"; s.replacement = 0x998;
    assert(ApiAddEnemySwap(mod, &s) == 0);
    assert(g_erEnemyCount == 4 && g_erSwapCount == 2 && EnemyRostersPending());
}

/* The Automaton roster (kRosters[2]) after the growth: 61 game rows as they were, then alpha's two. */
static void ErtCheckRows(uintptr_t game, const uint8_t* before)
{
    uint64_t pair[4];
    memcpy(pair, (void*)(game + kRosters[2].descriptor), sizeof pair);
    const uint8_t* rows = (const uint8_t*)(uintptr_t)pair[0];
    assert(rows != before && pair[1] == 63 && memcmp(rows, before, 61 * ER_ROW) == 0);
    const uint8_t* mine = rows + 61 * ER_ROW;
    uint64_t unit; uint32_t cap; float f;
    memcpy(&unit, mine + 8, 8); memcpy(&cap, mine + 0x18, 4); memcpy(&f, mine + 0x24, 4);
    assert(unit == ERT_UNIT && cap == 20 && f == 15.0f && memcmp(mine, before + 5 * ER_ROW, 8) == 0);
    memcpy(&f, mine + 0x30 + 4 * 2, 4); assert(f == 0);    /* level 3: below its lowest */
    memcpy(&f, mine + 0x30 + 4 * 3, 4); assert(f == 2.0f); /* level 4: 4 x 0.5 */
    memcpy(&f, mine + 0x30 + 4 * 9, 4); assert(f == 5.0f);
    memcpy(&unit, mine + ER_ROW + 8, 8); assert(unit == ERT_UNIT);
    int ready;
    assert(DirectorRow("EnemyRoster", (2u << 16) | 62, &ready) == (uintptr_t)mine + ER_ROW + 0x30 && ready);
    assert(DirectorRow("EnemyRoster", (2u << 16) | 63, &ready) == 0);
    /* its rule after the game's 39 */
    const uint8_t* rule = (const uint8_t*)(uintptr_t)pair[2] + 39 * ER_RULE;
    uint32_t tag, range[3]; uint64_t units[2]; float weights[2];
    assert(pair[3] == 40);
    memcpy(&tag, rule, 4); memcpy(units, rule + 8, 16); memcpy(weights, rule + 0x18, 8); memcpy(range, rule + 0x20, 12);
    assert(tag == 0xAE2ED4E9u && units[0] == ERT_UNIT && units[1] == ERT_SWAPPED && weights[0] == 1.0f && weights[1] == 15.0f);
    assert(range[0] == 1 && range[1] == 100 && range[2] == 5);
    /* the Terminid roster: alpha's row and the bug-only one; the Illuminate one: the game's rows only */
    memcpy(pair, (void*)(game + kRosters[0].descriptor), sizeof pair);
    assert(pair[1] == 47 && pair[3] == 22);
    memcpy(&unit, (const uint8_t*)(uintptr_t)pair[0] + 46 * ER_ROW + 8, 8); assert(unit == ERT_BUG_UNIT);
    memcpy(&f, (const uint8_t*)(uintptr_t)pair[0] + 46 * ER_ROW + 0x30, 4); assert(f == 1.0f); /* weight 0 means x 1 */
    memcpy(pair, (void*)(game + kRosters[1].descriptor), sizeof pair);
    assert(pair[1] == 46 && pair[3] == 3);
}

/* The budget's weight, through the stub as the game enters it (the unit in rdx). */
static void ErtCheckBudget(uintptr_t game, uint8_t* director)
{
    typedef float (*Budget)(uint64_t rcx, uint64_t unit);
    Budget budget = (Budget)(void*)g_erStub;
    uint8_t jump[14];
    uint64_t to = (uint64_t)(uintptr_t)g_erStub;
    memcpy(jump, (void*)(game + ER_BUDGET), sizeof jump);
    assert(jump[0] == 0xff && jump[1] == 0x25 && memcmp(jump + 6, &to, 8) == 0);
    assert(budget(0, ERT_LIKE) == 15.0f && budget(0, ERT_UNIT) == 15.0f); /* the Automaton roster's rows */
    assert(budget(0, ERT_SWAPPED) == 15.0f); /* its rule's */
    assert(budget(0, ERT_SHARED) == 2.0f && budget(0, 0x12345) == 0);
    uint32_t faction = 2;
    memcpy(director + ER_FACTION_AT, &faction, 4);
    assert(budget(0, ERT_LIKE) == 6.0f && budget(0, ERT_BUG_UNIT) == 6.0f);
    faction = 1; memcpy(director + ER_FACTION_AT, &faction, 4);
    assert(budget(0, ERT_LIKE) == 0 && budget(0, ERT_SHARED) == 2.0f); /* no faction roster: the shared ones only */
}

/* A row copied where level 11 moved the weights: 11 of them from +0x2C, level 11 judged as level 10. */
static void ErtMovedRow(void)
{
    uint8_t like[ER_ROW] = {0}, row[ER_ROW];
    ErEnemy e = {0};
    e.unit = ERT_UNIT; e.weight = 2.0f; e.lowest = 10;
    for (int l = 0; l < 11; l++) { float w = (float)(l + 1); memcpy(like + 0x2C + 4 * l, &w, 4); }
    ErRow(row, like, &e, 1);
    float f;
    memcpy(&f, row + 0x2C + 4 * 8, 4); assert(f == 0);
    memcpy(&f, row + 0x2C + 4 * 9, 4); assert(f == 20.0f);
    memcpy(&f, row + 0x2C + 4 * 10, 4); assert(f == 22.0f);
    e.lowest = 0; e.highest = 9;
    ErRow(row, like, &e, 1);
    memcpy(&f, row + 0x2C + 4 * 8, 4); assert(f == 18.0f);
    memcpy(&f, row + 0x2C + 4 * 10, 4); assert(f == 0);
}

static void EnemyRostersFixture(void)
{
    static const uint32_t rules[6] = {21, 3, 39, 0, 0, 0};
    HfMod* mod = &g_mods[0];
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, DIR_IMAGE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t* director = (uint8_t*)calloc(1, 0x52000);
    assert(image != NULL && director != NULL);
    uintptr_t game = (uintptr_t)image;
    uint8_t* rows[6];
    for (int r = 0; r < 6; r++)
    {
        rows[r] = (uint8_t*)calloc(kRosters[r].rows, ER_ROW);
        uint64_t pair[4] = {(uint64_t)(uintptr_t)rows[r], kRosters[r].rows, (uint64_t)(uintptr_t)calloc(rules[r] + 1, ER_RULE), rules[r]};
        memcpy(image + kRosters[r].descriptor, pair, sizeof pair);
    }
    ErtRow(rows[2] + 5 * ER_ROW, ERT_LIKE, 15.0f);
    ErtRow(rows[2] + 9 * ER_ROW, ERT_LIKE, 15.0f);
    ErtRow(rows[0] + 1 * ER_ROW, ERT_LIKE, 6.0f);
    ErtRow(rows[4] + 3 * ER_ROW, ERT_SHARED, 2.0f);
    image[kWeightReaders[0][0] + kWeightReaders[0][1]] = 0x30;
    memcpy(image + ER_BUDGET, kErBudgetPin, sizeof kErBudgetPin);
    uint64_t at = (uint64_t)(uintptr_t)director;
    uint32_t faction = 8;
    memcpy(image + ER_DIRECTOR, &at, 8);
    memcpy(director + ER_FACTION_AT, &faction, 4);
    LONG loaded = g_fired[HF_EVENT_GAME_LOADED], entities = g_fired[HF_EVENT_ENTITIES_READY];
    int added = g_difficultyAdded, state = g_difficultyState;
    g_difficultyAdded = 0; g_difficultyState = 1;
    g_directorImage = DirFakeImage;
    g_dirGame = image;
    g_erEntityKnown = ErtKnown;
    InterlockedExchange(&g_fired[HF_EVENT_GAME_LOADED], 1);
    InterlockedExchange(&g_fired[HF_EVENT_ENTITIES_READY], 0);

    ErtRefusals(mod);
    ErtAdd(mod);
    int ready = 1;
    assert(DirectorRow("EnemyRoster", (2u << 16) | 5, &ready) == 0 && !ready); /* held back until the rosters grew */
    WatchEnemyRosters(); /* the entity data has not loaded: nothing yet */
    assert(g_erState == 0);
    InterlockedExchange(&g_fired[HF_EVENT_ENTITIES_READY], 1);
    WatchEnemyRosters();
    assert(g_erState == 1 && !EnemyRostersPending());
    assert(ErEnemyAt(0)->state == 1 && ErEnemyAt(0)->rows == 3 && ErEnemyAt(1)->state == -1 && ErEnemyAt(2)->state == -1 && ErEnemyAt(3)->rows == 1);
    assert(ErSwapAt(0)->state == 1 && ErSwapAt(0)->rules == 2 && ErSwapAt(1)->state == -1);
    ErtCheckRows(game, rows[2]);
    ErtCheckBudget(game, director);
    ErtMovedRow();
    HfEnemy late = {sizeof late};
    late.name = "tester/enemies/late"; late.unit = ERT_UNIT; late.like = ERT_LIKE;
    assert(ApiAddEnemy(mod, &late) == 1);

    /* its mod taken out: no weight left in its rows, its rule out of every level's reach; the game's rows as they were */
    EnemyRostersForget(mod);
    static const uint8_t none[0x2C];
    uint32_t lowest;
    assert(memcmp(g_erGrown[1].rows + 61 * ER_ROW + 0x2C, none, sizeof none) == 0); /* g_erGrown[1]: the Automatons */
    assert(memcmp(g_erGrown[1].rows, rows[2], 61 * ER_ROW) == 0);
    memcpy(&lowest, g_erGrown[1].rules + 39 * ER_RULE + 0x20, 4); assert(lowest == ER_NEVER);

    g_erEntityKnown = ErEntityInMap;
    g_directorImage = DirectorGame;
    g_difficultyAdded = added; g_difficultyState = state;
    InterlockedExchange(&g_fired[HF_EVENT_GAME_LOADED], loaded);
    InterlockedExchange(&g_fired[HF_EVENT_ENTITIES_READY], entities);
    g_erGame = 0; /* the image goes; the copies stay, as in the game */
    for (int r = 0; r < 6; r++) free(rows[r]);
    free(director);
    VirtualFree(image, 0, MEM_RELEASE);
}
