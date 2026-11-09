/* Enemies mods add to the faction rosters (enemy_rosters.c) on a fake game.dll image: the refusals, the grown copies
 * (the game's rows first, a mod's row a scaled copy of its like's, rules after the game's), entries without an entity or
 * a row left out, the budget's weight through the stub, its pair table's guard, rule chains, level 11 added after the
 * growth, edits held back until the rosters grew, a mod taken out, units without their like's transport formations.
 * Included by stats_test.c after director_test.c. */

#define ERT_LIKE 0xAAAull
#define ERT_UNIT 0x111ull
#define ERT_BUG_UNIT 0x222ull
#define ERT_SWAPPED 0x333ull
#define ERT_SHARED 0x777ull
#define ERT_CARRIED 0xCCCull /* an enemy transports carry: it has a formation record */
#define ERT_RIDER 0xDDDull   /* another entity with that formation */

static int ErtKnown(uint64_t entity)
{
    return entity == ERT_LIKE || entity == ERT_UNIT || entity == ERT_BUG_UNIT || entity == ERT_SWAPPED || entity == ERT_CARRIED || entity == ERT_RIDER;
}

/* Formation records: the carried enemy's one formation, the rider's two (that one among them); none for the rest. */
static const uint8_t* ErtFormations(uint64_t entity)
{
    static const uint32_t carried[12] = {0x77}, rider[12] = {0x55, 0, 0, 0, 0, 0, 0x77};
    return entity == ERT_CARRIED ? (const uint8_t*)carried : entity == ERT_RIDER ? (const uint8_t*)rider : NULL;
}

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
    e.name = "tester/enemies/no_formation"; e.unit = ERT_UNIT; e.like = ERT_CARRIED; e.factions = 0;
    assert(ApiAddEnemy(mod, &e) == 0);
    e.name = "tester/enemies/rider"; e.unit = ERT_RIDER;
    assert(ApiAddEnemy(mod, &e) == 0);
    HfEnemySwap s = {sizeof s};
    s.name = "tester/enemies/assault"; s.unit = ERT_UNIT; s.replacement = ERT_SWAPPED; s.tag = "GM_BotAssault"; s.priority = 5;
    assert(ApiAddEnemySwap(mod, &s) == 0);
    s.name = "tester/enemies/no_replacement"; s.replacement = 0x998;
    assert(ApiAddEnemySwap(mod, &s) == 0);
    s.name = "tester/enemies/no_row_there"; s.replacement = ERT_SWAPPED; s.factions = HF_ENEMY_ILLUMINATE;
    assert(ApiAddEnemySwap(mod, &s) == 0);
    s.name = "tester/enemies/no_formation_rule"; s.unit = ERT_CARRIED; s.replacement = ERT_UNIT; s.factions = 0;
    assert(ApiAddEnemySwap(mod, &s) == 0);
    assert(g_erEnemyCount == 6 && g_erSwapCount == 4 && EnemyRostersPending());
}

/* The Automaton roster (kRosters[2]) after the growth: 61 game rows as they were, then alpha's two. */
static void ErtCheckRows(uintptr_t game, const uint8_t* before)
{
    uint64_t pair[4];
    memcpy(pair, (void*)(game + kRosters[2].descriptor), sizeof pair);
    const uint8_t* rows = (const uint8_t*)(uintptr_t)pair[0];
    assert(rows != before && pair[1] == 64 && memcmp(rows, before, 61 * ER_ROW) == 0); /* alpha's two, the rider's one */
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
    memcpy(&unit, mine + 2 * ER_ROW + 8, 8); assert(unit == ERT_RIDER);
    assert(DirectorRow("EnemyRoster", (2u << 16) | 64, &ready) == 0);
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

/* The budget's appends jump to their caves: the site's own load, the bound, back below it, else on without a pair. */
static void ErtCheckPairGuard(uintptr_t game)
{
    for (int i = 0; i < 2; i++)
    {
        const uint8_t* site = (const uint8_t*)(game + kErPairSites[i].site);
        const uint8_t* cave = g_erPairCave + i * ER_PAIR_CAVE;
        uint32_t n = kErPairSites[i].size;
        int32_t rel;
        memcpy(&rel, site + 1, 4);
        assert(site[0] == 0xe9 && site + 5 + rel == cave && site[5] == 0x90 && (n == 6 || site[6] == 0x90));
        assert(memcmp(site + n, kErPairSites[i].pin + n, 16 - n) == 0); /* what follows the load is untouched */
        assert(memcmp(cave, kErPairSites[i].pin, n) == 0 && cave[n] == 0x83 && cave[n + 1] == 0xf8 && cave[n + 2] == 64 && cave[n + 3] == 0x73);
        memcpy(&rel, cave + n + 6, 4);
        assert(cave[n + 5] == 0xe9 && (uintptr_t)(cave + n + 10 + rel) == game + kErPairSites[i].resume);
        memcpy(&rel, cave + n + 24, 4);
        assert(cave[n + 23] == 0xe9 && (uintptr_t)(cave + n + 28 + rel) == game + kErPairSites[i].skip);
    }
}

/* Rules in a row: three are the game's limit, a fourth or a loop is too long. */
static void ErtChains(void)
{
    uint8_t rules[5 * ER_RULE] = {0};
    for (uint64_t i = 0; i < 4; i++) { uint64_t from = 0x10 + i, to = 0x11 + i; memcpy(rules + i * ER_RULE + 8, &from, 8); memcpy(rules + i * ER_RULE + 0x10, &to, 8); }
    assert(!ErChainTooLong(rules, 3) && ErChainTooLong(rules, 4));
    uint64_t back = 0x10;
    memcpy(rules + 1 * ER_RULE + 0x10, &back, 8); /* 0x10 -> 0x11 -> 0x10 */
    assert(ErChainTooLong(rules, 2));
    uint32_t higher = 2;
    memcpy(rules + 0x28, &higher, 4); /* the way back has a lower priority: the game stops there */
    assert(!ErChainTooLong(rules, 2));
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
        uint8_t* rule = (uint8_t*)calloc(rules[r] + 1, ER_RULE);
        for (uint64_t i = 0; i < rules[r]; i++) { uint64_t from = 0x5000 + i, to = 0x6000 + i; memcpy(rule + i * ER_RULE + 8, &from, 8); memcpy(rule + i * ER_RULE + 0x10, &to, 8); }
        uint64_t pair[4] = {(uint64_t)(uintptr_t)rows[r], kRosters[r].rows, (uint64_t)(uintptr_t)rule, rules[r]};
        memcpy(image + kRosters[r].descriptor, pair, sizeof pair);
    }
    ErtRow(rows[2] + 5 * ER_ROW, ERT_LIKE, 15.0f);
    ErtRow(rows[2] + 9 * ER_ROW, ERT_LIKE, 15.0f);
    ErtRow(rows[0] + 1 * ER_ROW, ERT_LIKE, 6.0f);
    ErtRow(rows[4] + 3 * ER_ROW, ERT_SHARED, 2.0f);
    ErtRow(rows[2] + 20 * ER_ROW, ERT_CARRIED, 15.0f);
    image[kWeightReaders[0][0] + kWeightReaders[0][1]] = 0x30;
    memcpy(image + ER_BUDGET, kErBudgetPin, sizeof kErBudgetPin);
    for (int i = 0; i < 2; i++)
    {
        memcpy(image + kErPairSites[i].site, kErPairSites[i].pin, 16);
        memcpy(image + kErPairSites[i].skip, kErPairSites[i].skipPin, 5);
    }
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
    g_erFormations = ErtFormations;
    int wasLoaded = mod->loaded;
    mod->loaded = 1;
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
    assert(ErEnemyAt(4)->state == -1 && ErEnemyAt(5)->state == 1 && ErEnemyAt(5)->rows == 1 && ErSwapAt(3)->state == -1); /* formations */
    assert(ErSwapAt(0)->state == 1 && ErSwapAt(0)->rules == 2 && ErSwapAt(1)->state == -1 && ErSwapAt(2)->state == -1);
    ErtCheckPairGuard(game);
    ErtChains();
    ErtCheckRows(game, rows[2]);
    ErtCheckBudget(game, director);
    ErtMovedRow();
    /* level 11 added after the growth: the mod's rows move with the game's, level 11 taking level 10's weight */
    DifficultyInstall move = {0};
    move.game = game;
    float w10, w11, first;
    assert(MoveRosterWeights(&move) == NULL && DirectorWeightsMoved(game) == 1);
    memcpy(&first, g_erGrown[1].rows + 61 * ER_ROW + 0x2C + 4 * 3, 4);
    memcpy(&w10, g_erGrown[1].rows + 61 * ER_ROW + 0x2C + 4 * 9, 4);
    memcpy(&w11, g_erGrown[1].rows + 61 * ER_ROW + 0x54, 4);
    assert(first == 2.0f && w10 == 5.0f && w11 == 5.0f);
    HfEnemy late = {sizeof late};
    late.name = "tester/enemies/late"; late.unit = ERT_UNIT; late.like = ERT_LIKE;
    assert(ApiAddEnemy(mod, &late) == 1);

    /* its mod taken out: no weight left in its rows, its rule out of every level's reach; the game's rows as they were */
    static const uint8_t none[0x2C];
    static uint8_t before[61 * ER_ROW];
    uint32_t lowest;
    memcpy(before, g_erGrown[1].rows, sizeof before); /* g_erGrown[1]: the Automatons */
    EnemyRostersForget(mod);
    assert(memcmp(g_erGrown[1].rows + 61 * ER_ROW + 0x2C, none, sizeof none) == 0);
    assert(memcmp(g_erGrown[1].rows, before, sizeof before) == 0);
    memcpy(&lowest, g_erGrown[1].rules + 39 * ER_RULE + 0x20, 4); assert(lowest == ER_NEVER);

    g_erEntityKnown = ErEntityInMap;
    g_erFormations = ErFormationsOf;
    mod->loaded = wasLoaded;
    g_directorImage = DirectorGame;
    g_difficultyAdded = added; g_difficultyState = state;
    InterlockedExchange(&g_fired[HF_EVENT_GAME_LOADED], loaded);
    InterlockedExchange(&g_fired[HF_EVENT_ENTITIES_READY], entities);
    g_erGame = 0; /* the image goes; the copies stay, as in the game */
    for (int r = 0; r < 6; r++) free(rows[r]);
    free(director);
    VirtualFree(image, 0, MEM_RELEASE);
}
