/* Enemies mods add to the faction rosters (enemy_rosters.c) on a fake game.dll image: the refusals, the grown copies
 * (the game's rows first, a mod's row a scaled copy of its like's, rules after the game's), entries without an entity or
 * a row left out, the budget's weight through the stub, its pair table's guard, rule chains, level 11 added after the
 * growth, edits held back until the rosters grew, a mod taken out (once no mission runs); a row of the mod's own and its
 * squads, the guards for an enemy without Behavior or transport records, the group names, the plan arrays' bound.
 * Included by stats_test.c after director_test.c. */

#define ERT_LIKE 0xAAAull
#define ERT_UNIT 0x111ull
#define ERT_BUG_UNIT 0x222ull
#define ERT_SWAPPED 0x333ull
#define ERT_SHARED 0x777ull
#define ERT_CARRIED 0xCCCull /* an enemy transports carry: it has a formation record */
#define ERT_RIDER 0xDDDull   /* another entity with that formation */
#define ERT_PROP 0xEEEull    /* an entity that is no enemy of the game's */
#define ERT_GROUP "tester/enemies/own_group"

static int ErtKnown(uint64_t entity)
{
    return entity == ERT_LIKE || entity == ERT_UNIT || entity == ERT_BUG_UNIT || entity == ERT_SWAPPED || entity == ERT_CARRIED || entity == ERT_RIDER || entity == ERT_PROP;
}

/* The game's seat entry of a formation: a 6 x 2 one of node kind 3. */
static const uint8_t* ErtSeat(uint32_t id)
{
    static uint32_t entry[6];
    entry[0] = id; entry[1] = 6; entry[2] = 2; entry[3] = 7; entry[4] = 3;
    return (const uint8_t*)entry;
}

static uint64_t ErtNext(void* a, void* roster, const uint8_t* rows, uint32_t count, uint8_t* out, uint32_t* outCount, uint64_t level,
    uint64_t players, void* tags, uint64_t flagA, uint64_t flagB)
{
    for (uint32_t i = 0; i < count; i++) memcpy(out + (size_t)i * ES_ENTRY, &i, 4); /* every row fits the mission */
    *outCount = count;
    return 7;
}
static const char* ErtHook(uintptr_t game) { g_esSelectOriginal = (void*)ErtNext; return NULL; }

/* What the guards patch, on the fake image: the seven calls, the six name reads, and two lookups that run (Behavior's
 * finds no record; the formations' gives back its argument as the record). The squad families of two rosters. */
static void ErtGuardsImage(uint8_t* image)
{
    static const uint8_t none[] = {0x31,0xc0,0xc3}, same[] = {0x48,0x89,0xc8,0xc3}; /* xor eax, eax; ret / mov rax, rcx; ret */
    for (size_t i = 0; i < EG_CALLS; i++) memcpy(image + kEgCalls[i].site, kEgCalls[i].pin, 5);
    for (uint32_t f = 0; f < ER_FACTIONS; f++) for (int k = 0; k < 2; k++) memcpy(image + kEgNameReads[f][k], &kErNames[f], 4);
    memcpy(image + EG_BEHAVIOR, none, sizeof none);
    memcpy(image + EG_FORMATION, same, sizeof same);
    DWORD previous;
    VirtualProtect(image + EG_FORMATION, 0x20000, PAGE_EXECUTE_READWRITE, &previous); /* both lookups */
    for (int k = 0; k < 8; k++)
    {
        uint64_t pair[2] = {(uint64_t)(uintptr_t)calloc(80, ES_ROW), k == 0 ? 79 : k == 2 ? 17 : 1};
        memcpy(image + kRosters[1].descriptor + 0x20 + 0x10 * k, pair, sizeof pair);
        pair[0] = (uint64_t)(uintptr_t)calloc(64, ES_ROW); pair[1] = k == 2 ? 27 : 1;
        memcpy(image + kRosters[0].descriptor + 0x20 + 0x10 * k, pair, sizeof pair);
    }
}

/* The stubs as the game's calls enter them: no Behavior record gives the zeroed one; a formation record with the id
 * comes back as it is, one without it (or none) gives a single seat placed as the game's entry of that formation. */
static void ErtGuards(uintptr_t game)
{
    typedef const uint8_t* (*Lookup)(const void* entity);
    typedef const uint8_t* (*ByRegister)(const void* entity, uint32_t id, Lookup stub);
    static const uint8_t thunk[] = {0x53, 0x89,0xd3, 0x48,0x83,0xec,0x20, 0x41,0xff,0xd0, 0x48,0x83,0xc4,0x20, 0x5b, 0xc3}; /* ebx = edx; call r8 */
    Lookup stubs[EG_CALLS];
    for (size_t i = 0; i < EG_CALLS; i++)
    {
        int32_t rel;
        uint8_t* site = (uint8_t*)(game + kEgCalls[i].site);
        memcpy(&rel, site + 1, 4);
        assert(site[0] == 0xe8);
        stubs[i] = (Lookup)(void*)(site + 5 + rel);
    }
    assert(stubs[0] == stubs[1] && stubs[3] == stubs[4] && stubs[5] == stubs[6] && stubs[3] != stubs[5]);
    assert(stubs[0](NULL) == g_egNoBehavior);
    uint32_t has[12] = {0x11, 2, 2, 0, 0, 0, EG_FORMATION_ID, 1, 1}, hasNot[12] = {0x11, 2, 2}, seat[6];
    assert(stubs[5](has) == (const uint8_t*)has);
    const uint8_t* made = stubs[5](hasNot);
    memcpy(seat, made, sizeof seat);
    assert(made != (const uint8_t*)hasNot && seat[0] == EG_FORMATION_ID && seat[1] == 1 && seat[2] == 1 && seat[3] == 7 && seat[4] == 3);
    assert(stubs[5](NULL) == made && g_egSeatCount == 1); /* kept per formation */
    uint8_t* code = (uint8_t*)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(code, thunk, sizeof thunk);
    ByRegister byRegister = (ByRegister)(void*)code;
    assert(byRegister(has, 0x11, stubs[3]) == (const uint8_t*)has && byRegister(has, EG_FORMATION_ID, stubs[3]) == (const uint8_t*)has);
    made = byRegister(has, 0x22, stubs[3]);
    memcpy(seat, made, sizeof seat);
    assert(made != (const uint8_t*)has && seat[0] == 0x22 && seat[1] == 1 && seat[4] == 3 && g_egSeatCount == 2);
}

/* The plan builder's detour: a family that fits its plan array goes straight on; a larger one is cut to the array's
 * size, spread over the rows that fit; an array that is not the plan's is left alone. */
static void ErtSelect(void)
{
    static uint8_t plan[0x9000];
    uint32_t* count = (uint32_t*)(void*)(plan + 0x8DE0); /* the air support array: 8 entries */
    uint32_t first, last, own = 0;
    assert(EsSelect(NULL, NULL, NULL, 5, plan, count, (uint64_t)0, (uint64_t)0, NULL, (uint64_t)0, (uint64_t)0) == 7 && *count == 5);
    assert(EsSelect(NULL, NULL, NULL, 20, plan, count, (uint64_t)0, (uint64_t)0, NULL, (uint64_t)0, (uint64_t)0) == 7 && *count == 8 && g_esLeftOut == 12);
    memcpy(&first, plan, 4); memcpy(&last, plan + 7 * ES_ENTRY, 4);
    assert(first < last && last == 19);
    static uint8_t other[20 * ES_ENTRY];
    assert(EsSelect(NULL, NULL, NULL, 20, other, &own, (uint64_t)0, (uint64_t)0, NULL, (uint64_t)0, (uint64_t)0) == 7 && own == 20);
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

/* An enemy with a row of its own in the Illuminate roster, tied to no game enemy, and squads of its group. */
static void ErtAddOwn(HfMod* mod)
{
    HfEnemy e = {sizeof e};
    e.name = "tester/enemies/own"; e.unit = ERT_PROP;
    assert(ApiAddEnemy(mod, &e) == 1); /* no like: it needs its factions and a group */
    e.factions = HF_ENEMY_ILLUMINATE; e.group = ERT_GROUP;
    assert(ApiAddEnemy(mod, &e) == 1); /* no weight at any level */
    e.weights[4] = 2.0f; e.weights[9] = 3.0f; e.size_class = 4;
    assert(ApiAddEnemy(mod, &e) == 1);
    e.size_class = 2; e.cost = 40.0f; e.alive_cap = 6; e.flags = HF_ENEMY_ALWAYS_LOADED; e.weight = 2.0f; e.highest_level = 9;
    e.with_tags[1] = "GM_BotAssault"; e.without_tags[0] = "GM_SEAF";
    assert(ApiAddEnemy(mod, &e) == 0);
    HfEnemySquad s = {sizeof s};
    s.name = "tester/enemies/own_patrol"; s.factions = HF_ENEMY_ILLUMINATE; s.kind = HF_SQUAD_PATROL; s.weight = 1.5f;
    assert(ApiAddEnemySquad(mod, &s) == 1); /* no member */
    s.members[1].group = ERT_GROUP; s.members[1].count = 33;
    assert(ApiAddEnemySquad(mod, &s) == 1); /* over 32 enemies */
    s.members[1].count = 3; s.kind = 2;
    assert(ApiAddEnemySquad(mod, &s) == 1); /* no such kind */
    s.kind = HF_SQUAD_PATROL; s.lowest_level = 5; s.with_tags[2] = "GM_SEAF";
    assert(ApiAddEnemySquad(mod, &s) == 0);
    assert(ApiAddEnemySquad(mod, &s) == 1); /* its name is taken */
    s.name = "tester/enemies/no_group"; s.members[1].group = "tester/enemies/nobody";
    assert(ApiAddEnemySquad(mod, &s) == 0); /* refused when the rosters grow: no enemy in its group */
    s.name = "tester/enemies/pool"; s.kind = HF_SQUAD_OUTPOST; s.members[1].group = ERT_GROUP; s.members[1].count = 999; s.factions = HF_ENEMY_ILLUMINATE | HF_ENEMY_TERMINIDS;
    assert(ApiAddEnemySquad(mod, &s) == 0); /* the Terminid roster has no enemy of the group: Illuminate only */
    assert(g_erEnemyCount == 6 && g_erSquadCount == 3);
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
    e.name = "tester/enemies/rider"; e.unit = ERT_RIDER; e.like = ERT_CARRIED; e.factions = 0; /* any entity will do */
    assert(ApiAddEnemy(mod, &e) == 0);
    HfEnemySwap s = {sizeof s};
    s.name = "tester/enemies/assault"; s.unit = ERT_UNIT; s.replacement = ERT_SWAPPED; s.tag = "GM_BotAssault"; s.priority = 5;
    assert(ApiAddEnemySwap(mod, &s) == 0);
    s.name = "tester/enemies/no_replacement"; s.replacement = 0x998;
    assert(ApiAddEnemySwap(mod, &s) == 0);
    s.name = "tester/enemies/no_row_there"; s.replacement = ERT_SWAPPED; s.factions = HF_ENEMY_ILLUMINATE;
    assert(ApiAddEnemySwap(mod, &s) == 0);
    assert(g_erEnemyCount == 5 && g_erSwapCount == 3 && EnemyRostersPending());
    ErtAddOwn(mod);
}

/* The row of its own: every field as given, its weights x 2 up to level 9; its group's name where the game's name reads
 * now look; its squads after the game's in their families. */
static void ErtCheckOwn(uintptr_t game, const uint8_t* row)
{
    uint32_t key = ErHash(ERT_GROUP), u[4]; uint64_t unit; float f[2], w[10];
    memcpy(u, row, 4); memcpy(&unit, row + 8, 8); memcpy(u + 1, row + 0x10, 12); memcpy(f, row + 0x20, 8); memcpy(w, row + 0x30, sizeof w);
    assert(u[0] == key && unit == ERT_PROP && u[1] == 2 && u[2] == 0 && u[3] == 6 && f[0] == 40.0f && f[1] == 1.0f);
    assert(w[3] == 0 && w[4] == 4.0f && w[8] == 0 && w[9] == 0 && row[0x78] == 1);
    memcpy(u, row + 0x58, 16); assert(u[0] == 0xAE2ED4E9u && u[1] == 0);
    memcpy(u, row + 0x68, 16); assert(u[0] == ErHash("GM_SEAF") && u[1] == 0);
    int32_t disp;
    memcpy(&disp, (void*)(game + kEgNameReads[2][0]), 4);
    const char** names = (const char**)(game + (intptr_t)disp);
    assert(names == g_erGrown[2].names && !strcmp(names[46], ERT_GROUP) && names[0] == NULL);
    memcpy(&disp, (void*)(game + kEgNameReads[0][1]), 4);
    assert((const char**)(game + (intptr_t)disp) == g_erGrown[0].names);
    uint64_t pair[2];
    memcpy(pair, (void*)(game + kRosters[1].descriptor + 0x20), sizeof pair);
    const uint8_t* squad = (const uint8_t*)(uintptr_t)pair[0] + 79 * ES_ROW;
    assert(pair[1] == 80 && EnemySquadsIn(1, 0) == 1 && EnemySquadsIn(1, 2) == 1 && EnemySquadsIn(0, 2) == 0);
    memcpy(u, squad, 12); memcpy(f, squad + 0xE0, 8);
    assert(u[0] == key && u[1] == 3 && u[2] == 0 && f[0] == 1.5f && f[1] == 1.0f);
    memcpy(u, squad + 0xEC, 16); assert(u[0] == 5 && u[1] == 10 && u[2] == 1 && u[3] == 4);
    memcpy(u, squad + 0x60, 8); assert(u[0] == ErHash("GM_SEAF") && u[1] == 0);
    memcpy(&unit, squad + 0x100, 8); memcpy(u, squad + 0xFC, 4); memcpy(u + 1, squad + 0x110, 4);
    assert(unit == ES_SQUAD_ENTITY && u[0] == ES_ID && u[1] == ES_PATROL_KIND);
    memcpy(pair, (void*)(game + kRosters[1].descriptor + 0x40), sizeof pair);
    squad = (const uint8_t*)(uintptr_t)pair[0] + 17 * ES_ROW;
    memcpy(u, squad, 8); memcpy(&unit, squad + 0x100, 8); memcpy(u + 2, squad + 0xFC, 4);
    assert(pair[1] == 18 && u[0] == key && u[1] == 999 && unit == 0 && u[2] == key);
    assert(EsAt(0)->state == 1 && EsAt(1)->state == -1 && EsAt(2)->state == 1 && EsAt(2)->rows == 1);
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
    assert(pair[1] == 47 && pair[3] == 3);
    ErtCheckOwn(game, (const uint8_t*)(uintptr_t)pair[0] + 46 * ER_ROW);
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
    g_egSeat = ErtSeat;
    g_esHook = ErtHook;
    ErtGuardsImage(image);
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
    assert(ErEnemyAt(4)->state == 1 && ErEnemyAt(4)->rows == 1 && ErEnemyAt(5)->state == 1 && ErEnemyAt(5)->rows == 1);
    assert(ErSwapAt(0)->state == 1 && ErSwapAt(0)->rules == 2 && ErSwapAt(1)->state == -1 && ErSwapAt(2)->state == -1);
    ErtGuards(game);
    ErtSelect();
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
    uint32_t running = 1, squadKey;
    memcpy(director + 0x680, &running, 4);
    EnemyRostersForget(mod); /* a mission runs: its plan may hold their squads, so nothing changes yet */
    assert(memcmp(g_erGrown[1].rows + 61 * ER_ROW + 0x2C, none, sizeof none) != 0);
    running = 0;
    memcpy(director + 0x680, &running, 4);
    WatchEnemyRosters();
    memcpy(&squadKey, g_esGrown[2][0].rows + 79 * ES_ROW, 4);
    assert(squadKey == 0 && memcmp(g_erGrown[2].rows + 46 * ER_ROW + 0x2C, none, sizeof none) == 0);
    assert(memcmp(g_erGrown[1].rows + 61 * ER_ROW + 0x2C, none, sizeof none) == 0);
    assert(memcmp(g_erGrown[1].rows, before, sizeof before) == 0);
    memcpy(&lowest, g_erGrown[1].rules + 39 * ER_RULE + 0x20, 4); assert(lowest == ER_NEVER);

    g_erEntityKnown = ErEntityInMap;
    g_egSeat = EgSeatInGame;
    g_esHook = EsHookGame;
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
