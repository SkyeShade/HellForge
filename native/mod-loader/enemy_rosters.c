/* Enemies mods add to the spawn director's faction rosters (SDK 17: add_enemy, add_enemy_swap; docs/modding/enemies.md).
 * Included by director.c at its end (kRosters, DirectorWeightsMoved, g_directorImage); the entity map is registry.c's.
 *
 * A roster descriptor (kRosters, read-only data) holds {rows, count} for its unit rows (0x80 bytes) at +0 and for its
 * replacement rules (0x30) at +0x10. The weighted pick (0x953150), the replacement (0x949B80) and the mission's package
 * loader (0xABCB30) reach a faction's rows and rules only through those pairs, to the count. So each faction roster
 * gets a larger copy, the game's rows first (their indices stay), and its pairs point to it. The copies are never
 * freed: a reader may hold one. The shared rosters stay as they are: the package loader walks one of them by a fixed
 * address and count (0xABD7A2), so a row added there would spawn without its packages. */

#define ER_CEILING 65536u      /* address space for the mods' entries (stable_array.c), not a game limit */
#define ER_ROW 0x80u
#define ER_RULE 0x30u
#define ER_ROWS_MAX 0xFFFFu    /* a roster row's id holds its index in 16 bits (director.c) */
#define ER_FACTIONS 3u
#define ER_DIRECTOR 0x3326D10u /* the HiveMind director */
#define ER_FACTION_AT 0x518BCu /* its mission's faction: 2, 8 or 4 */
#define ER_BUDGET 0x94BA40u
#define ER_NEVER 0xFFFFFFFFu   /* a rule's lowest level that no level reaches */
#define ER_CHAIN 3u            /* replacements in a row the game takes; a fourth sends 0x949B80 down its error path */
#define ER_PAIRS 64u           /* the budget's pairs of unit type and spawn source its lookups reach */
#define ER_WAIT_MS 120000u     /* for the entity data, as the director's stats pass waits */

/* The faction rosters: kRosters' index, the SDK's bit, the director's faction id, the game's rule count. */
static const struct { uint32_t roster, bit, faction, rules; } kErFactions[ER_FACTIONS] = {
    {0, HF_ENEMY_TERMINIDS, 2, 21}, {2, HF_ENEMY_AUTOMATONS, 8, 39}, {1, HF_ENEMY_ILLUMINATE, 4, 3}};
/* The rosters every faction shares, in the order the budget's scan takes them after the faction's own. */
static const uint32_t kErShared[3] = {4, 3, 5};
/* Each faction's group names, a char* per game row (0x94B570 and 0xAD51B0 index them by a row's number). */
static const uint32_t kErNames[ER_FACTIONS] = {0x21C8E50u, 0x21C8910u, 0x21C9A60u};
static const uint8_t kErBudgetPin[] = {0x48, 0x8b, 0x05, 0xc9, 0xb2, 0x9d, 0x02, 0x8b, 0x88, 0xbc, 0x18, 0x05, 0x00, 0x33, 0xc0, 0x83,
    0xf9, 0x02, 0x0f, 0x85, 0xbe, 0x00, 0x00, 0x00, 0x8b, 0xc8};

/* state: 0 waiting for the rosters to grow, 1 in them (rows: how many it got), -1 refused then. */
typedef struct ErEnemy
{
    HfMod* mod;
    char name[192];
    uint64_t unit, like; /* like 0: a row of its own, from the fields after `rows` */
    float weight;
    uint32_t factions, lowest, highest;
    int state, rows, gone; /* gone: its mod was taken out */
    char group[96];
    uint32_t key, cap, sizeClass, flags, with[4], without[4];
    uint64_t packages[2];
    float weights[DIF_VANILLA], cost, budget;
} ErEnemy;
typedef struct ErSwap
{
    HfMod* mod;
    char name[192];
    uint64_t unit, replacement;
    uint32_t factions, tag, lowest, highest, priority;
    float budget, cost;
    int state, rules, gone;
} ErSwap;
/* A faction's grown arrays; the mod entry (its index) behind each row and rule after the game's. */
typedef struct ErGrown { uint8_t* rows; uint8_t* rules; uint64_t rowCount, ruleCount; int* rowOwner; int* ruleOwner; const char** names; } ErGrown;

static StableArray g_erEnemyStore = {NULL, sizeof(ErEnemy), ER_CEILING, 0};
static StableArray g_erSwapStore = {NULL, sizeof(ErSwap), ER_CEILING, 0};
static int g_erEnemyCount, g_erSwapCount;
static int g_erState; /* 0 not grown yet, 1 grown, -1 not (logged) */
static ErGrown g_erGrown[ER_FACTIONS];
static uintptr_t g_erGame;
static uint8_t* g_erStub;
static uint8_t* g_erPairCave;
static ULONGLONG g_erSince, g_erPairsReportedAt;
static volatile LONG g_erPairsFull;
static LONG g_erPairsFullReported;

static int JournalPut(uintptr_t address, const uint8_t* bytes, size_t size); /* live_code.c */

static ErEnemy* ErEnemyAt(int i) { return (ErEnemy*)(g_erEnemyStore.base + (size_t)i * sizeof(ErEnemy)); }
static ErSwap* ErSwapAt(int i) { return (ErSwap*)(g_erSwapStore.base + (size_t)i * sizeof(ErSwap)); }

/* Whether the rosters are still to grow: director.c holds roster edits back until then, so they land in the copies. */
static int g_erSquadCount;
static int EnemyRostersPending(void) { return g_erState == 0 && (g_erEnemyCount > 0 || g_erSwapCount > 0 || g_erSquadCount > 0); }
static const char* EnemyGuardsInstall(uintptr_t game);          /* enemy_guards.c */
static const char* EnemyNamesInstall(uintptr_t game);
static void EnemyGuardsWatch(void);
static const char* EnemySquadsGrow(uintptr_t game, int moved);  /* enemy_squads.c */
static void EnemySquadsForget(void);
static void EnemySquadsMark(const HfMod* mod);
static void EnemySquadsWatch(void);
static void EnemySquadsTell(const char* why);
static uint64_t EnemySquadsAdded(void);
static void ErForgetGone(void);
static int g_erForgetting; /* entries of mods taken out wait for the mission's end */

/* Whether the game has this entity: in its entity map, and not one of a mod's that was refused. */
static int ErEntityInMap(uint64_t entity)
{
    uint8_t* map = StatePointer(EntityState(), REG_MAP);
    return map != NULL && FindRow(map, REG_MAP_SLOTS, 32, entity) != NULL && !RefusedEntity(entity);
}
/* The native fixture replaces it. */
static int (*g_erEntityKnown)(uint64_t entity) = ErEntityInMap;

/* ---- the API -------------------------------------------------------------------------------------------------------- */

static HfMod* ErNamed(const char* name)
{
    for (int i = 0; i < g_erEnemyCount; i++) if (!strcmp(ErEnemyAt(i)->name, name)) return ErEnemyAt(i)->mod;
    for (int i = 0; i < g_erSwapCount; i++) if (!strcmp(ErSwapAt(i)->name, name)) return ErSwapAt(i)->mod;
    return NULL;
}

/* What every entry must have; 1 when refused (logged as `what`). */
static int ErRefused(HfMod* mod, const char* what, const char* name, uint32_t factions)
{
    if (name == NULL || name[0] == 0 || strlen(name) >= sizeof ErEnemyAt(0)->name) { ApiLog(mod, "%s: it needs a name (under 192 bytes)", what); return 1; }
    if (g_erState != 0 || g_fired[HF_EVENT_ENTITIES_READY])
    { ApiLog(mod, "%s(%s): too late; the rosters are made when the game starts (call it from hellforge_mod_init; not in a live set-up)", what, name); return 1; }
    if (factions & ~(HF_ENEMY_TERMINIDS | HF_ENEMY_AUTOMATONS | HF_ENEMY_ILLUMINATE))
    { ApiLog(mod, "%s(%s): its factions are HF_ENEMY_TERMINIDS, HF_ENEMY_AUTOMATONS and HF_ENEMY_ILLUMINATE", what, name); return 1; }
    HfMod* other = ErNamed(name);
    if (other != NULL) { ApiLog(mod, "%s(%s): already added by %s", what, name, other->id); return 1; }
    return 0;
}

static int ErLevelsRefused(HfMod* mod, const char* what, const char* name, uint32_t lowest, uint32_t highest, uint32_t top)
{
    if (lowest <= top && highest <= top && (lowest == 0 || highest == 0 || lowest <= highest)) return 0;
    ApiLog(mod, "%s(%s): its levels are 1 to %u, the lowest not above the highest", what, name, top);
    return 1;
}

static uint32_t ErHash(const char* text) { return text != NULL && text[0] != 0 ? (uint32_t)(Murmur64A(text, strlen(text)) >> 32) : 0; }

/* Four tags' hashes with no gap: the game's lists end at their first 0 (0x177E3C0). */
static void ErTags(uint32_t* tags, const char* const* names)
{
    for (int i = 0, n = 0; i < 4; i++) if (ErHash(names[i])) tags[n++] = ErHash(names[i]);
}

static int ErWeightsRefused(HfMod* mod, const HfEnemy* def, const char* name, ErEnemy* e)
{
    float most = 0;
    for (uint32_t i = 0; i < DIF_VANILLA; i++)
    {
        if (!isfinite(def->weights[i]) || def->weights[i] < 0 || def->weights[i] > 1000) { ApiLog(mod, "add_enemy(%s): its weights are 0 to 1000", name); return 1; }
        e->weights[i] = def->weights[i];
        most = def->weights[i] > most ? def->weights[i] : most;
    }
    if (most <= 0) { ApiLog(mod, "add_enemy(%s): it needs a weight above 0 at some level", name); return 1; }
    return 0;
}

/* An enemy with a row of its own (no `like`): its fields into e, or 1 when one is refused. A group's key is the game's
 * hash of its name, as its tags' are; key 0 ends a squad's member list, so no group may hash to it. */
static int ErOwnRefused(HfMod* mod, const HfEnemy* def, const char* name, ErEnemy* e)
{
    if (def->size < offsetof(HfEnemy, without_tags) + sizeof def->without_tags) { ApiLog(mod, "add_enemy(%s): without `like` it needs the whole HfEnemy (its size is too small)", name); return 1; }
    if (def->factions == 0 || ErHash(def->group) == 0 || strlen(def->group) >= sizeof e->group)
    { ApiLog(mod, "add_enemy(%s): without `like` it needs its factions and a group (a name under 96 bytes)", name); return 1; }
    if (ErWeightsRefused(mod, def, name, e)) return 1;
    if (!isfinite(def->cost) || def->cost < 0 || def->cost > 100000 || !isfinite(def->budget) || def->budget < 0 || def->budget > 1000 ||
        def->alive_cap > 100000 || def->size_class > 3 || (def->flags & ~HF_ENEMY_ALWAYS_LOADED))
    { ApiLog(mod, "add_enemy(%s): its cost is 0 to 100000, its budget 0 to 1000, its cap up to 100000, its size class 0 to 3", name); return 1; }
    snprintf(e->group, sizeof e->group, "%s", def->group);
    e->key = ErHash(def->group);
    e->cost = def->cost; e->budget = def->budget > 0 ? def->budget : 1.0f;
    e->cap = def->alive_cap; e->sizeClass = def->size_class; e->flags = def->flags;
    ErTags(e->with, def->with_tags);
    ErTags(e->without, def->without_tags);
    return 0;
}

/* add_enemy (SDK 17): kept until the entity data loads, when the rosters grow (ErGrow). */
static int ApiAddEnemy(HfMod* mod, const HfEnemy* def)
{
    if (mod == NULL || def == NULL || def->size < offsetof(HfEnemy, highest_level) + sizeof def->highest_level)
    { ApiLog(mod, "add_enemy: no enemy, or its size is not set (HfEnemy e = {sizeof e})"); return 1; }
    char identity[256];
    const char* name = Identity(mod, def->name, identity, sizeof identity);
    if (ErRefused(mod, "add_enemy", name, def->factions) ||
        ErLevelsRefused(mod, "add_enemy", name, def->lowest_level, def->highest_level, DIF_VANILLA)) return 1;
    if (def->unit == 0) { ApiLog(mod, "add_enemy(%s): it needs a unit", name); return 1; }
    if (!isfinite(def->weight) || def->weight < 0 || def->weight > 1000) { ApiLog(mod, "add_enemy(%s): its weight is 0 to 1000", name); return 1; }
    if (!StableRoom(&g_erEnemyStore, (size_t)g_erEnemyCount + 1)) { ApiLog(mod, "add_enemy(%s): no memory for more enemies", name); return 1; }
    ErEnemy e = {0};
    e.mod = mod;
    snprintf(e.name, sizeof e.name, "%s", name);
    e.unit = def->unit; e.like = def->like;
    e.weight = def->weight > 0 ? def->weight : 1.0f;
    e.factions = def->factions; e.lowest = def->lowest_level; e.highest = def->highest_level;
    if (e.like == 0 && ErOwnRefused(mod, def, name, &e)) return 1;
    if (def->size >= offsetof(HfEnemy, packages) + sizeof def->packages) memcpy(e.packages, def->packages, sizeof e.packages);
    *ErEnemyAt(g_erEnemyCount) = e;
    g_erEnemyCount++;
    if (e.like) ApiLog(mod, "adding enemy %s: %016llx beside %016llx, its weights x %g", name, (unsigned long long)e.unit, (unsigned long long)e.like, e.weight);
    else ApiLog(mod, "adding enemy %s: %016llx in the group %s (%08x)", name, (unsigned long long)e.unit, e.group, e.key);
    return 0;
}

/* add_enemy_swap (SDK 17): a rule, kept as add_enemy's rows are. Its tag is the game's hash of the tag's name. */
static int ApiAddEnemySwap(HfMod* mod, const HfEnemySwap* def)
{
    if (mod == NULL || def == NULL || def->size < offsetof(HfEnemySwap, budget) + sizeof def->budget)
    { ApiLog(mod, "add_enemy_swap: no rule, or its size is not set (HfEnemySwap s = {sizeof s})"); return 1; }
    char identity[256];
    const char* name = Identity(mod, def->name, identity, sizeof identity);
    if (ErRefused(mod, "add_enemy_swap", name, def->factions) ||
        ErLevelsRefused(mod, "add_enemy_swap", name, def->lowest_level, def->highest_level, 100)) return 1;
    if (def->unit == 0 || def->replacement == 0 || def->unit == def->replacement)
    { ApiLog(mod, "add_enemy_swap(%s): it needs an enemy and another to replace it", name); return 1; }
    if (!isfinite(def->budget) || def->budget < 0 || def->budget > 1000 || def->priority > 1000)
    { ApiLog(mod, "add_enemy_swap(%s): its budget is 0 to 1000 and its priority 0 to 1000", name); return 1; }
    if (!StableRoom(&g_erSwapStore, (size_t)g_erSwapCount + 1)) { ApiLog(mod, "add_enemy_swap(%s): no memory for more rules", name); return 1; }
    ErSwap s = {0};
    s.mod = mod;
    snprintf(s.name, sizeof s.name, "%s", name);
    s.unit = def->unit; s.replacement = def->replacement;
    s.factions = def->factions; s.lowest = def->lowest_level; s.highest = def->highest_level;
    s.priority = def->priority; s.budget = def->budget;
    if (def->size >= offsetof(HfEnemySwap, cost) + sizeof def->cost && isfinite(def->cost) && def->cost > 0 && def->cost <= 100) s.cost = def->cost;
    if (def->tag != NULL && def->tag[0] != 0) s.tag = (uint32_t)(Murmur64A(def->tag, strlen(def->tag)) >> 32);
    *ErSwapAt(g_erSwapCount) = s;
    g_erSwapCount++;
    ApiLog(mod, "adding enemy rule %s: %016llx for %016llx%s%s", name, (unsigned long long)s.replacement, (unsigned long long)s.unit,
        s.tag ? " under " : "", s.tag ? def->tag : "");
    return 0;
}

/* ---- the budget's weight of a unit ---------------------------------------------------------------------------------- */

/* A roster's row of `unit` (+0x24), else with `rules` its rule that spawns it (+0x1C): 1 with *weight, 0 none. */
static int ErBudgetIn(uintptr_t descriptor, uint64_t unit, int rules, float* weight)
{
    uint64_t pair[4];
    memcpy(pair, (const void*)descriptor, sizeof pair);
    const uint8_t* row = (const uint8_t*)(uintptr_t)pair[0];
    for (uint64_t i = 0; row != NULL && i < pair[1]; i++, row += ER_ROW)
        if (memcmp(row + 8, &unit, 8) == 0) { memcpy(weight, row + 0x24, 4); return 1; }
    const uint8_t* rule = (const uint8_t*)(uintptr_t)pair[2];
    for (uint64_t i = 0; rules && rule != NULL && i < pair[3]; i++, rule += ER_RULE)
        if (memcmp(rule + 0x10, &unit, 8) == 0) { memcpy(weight, rule + 0x1C, 4); return 1; }
    return 0;
}

/* The game's 0x94BA40 through the pairs: the weight AiEnemyBudget keeps for a unit type it meets (0xA2BE44, 0xA2D6C3).
 * The game's scans the rows and rules at their fixed addresses and counts, which would give a mod's enemy 0. The
 * mission's faction roster first, its rules after its rows, then the shared rosters' rows. */
static float ErBudget(uint64_t unit)
{
    uintptr_t game = g_erGame, director = 0;
    uint32_t faction = 0;
    float weight = 0;
    memcpy(&director, (const void*)(game + ER_DIRECTOR), 8);
    if (director != 0) memcpy(&faction, (const void*)(director + ER_FACTION_AT), 4);
    for (uint32_t f = 0; f < ER_FACTIONS; f++)
        if (kErFactions[f].faction == faction && ErBudgetIn(game + kRosters[kErFactions[f].roster].descriptor, unit, 1, &weight)) return weight;
    for (uint32_t s = 0; s < 3; s++)
        if (ErBudgetIn(game + kRosters[kErShared[s]].descriptor, unit, 0, &weight)) return weight;
    return 0;
}

/* The game's function is a leaf that takes the unit in rdx and touches rax, rcx, rdx, r8 and xmm0 only, and its
 * callers keep r9 across the call. So the stub saves every other volatile register around ErBudget. Entered by a jump
 * from the function's entry: three pushes and 0x70 bytes align the stack for the call. */
static void ErBudgetStub(uint8_t* c)
{
    static const uint8_t save[] = {0x41,0x51, 0x41,0x52, 0x41,0x53, 0x48,0x83,0xec,0x70,           /* push r9, r10, r11; sub rsp, 0x70 */
        0xf3,0x0f,0x7f,0x4c,0x24,0x20, 0xf3,0x0f,0x7f,0x54,0x24,0x30, 0xf3,0x0f,0x7f,0x5c,0x24,0x40, /* movdqu [rsp+..], xmm1..3 */
        0xf3,0x0f,0x7f,0x64,0x24,0x50, 0xf3,0x0f,0x7f,0x6c,0x24,0x60,                               /* ... xmm4, xmm5 */
        0x48,0x89,0xd1, 0x48,0xb8};                                                                   /* mov rcx, rdx; mov rax, imm64 */
    static const uint8_t restore[] = {0xff,0xd0,                                                      /* call rax */
        0xf3,0x0f,0x6f,0x4c,0x24,0x20, 0xf3,0x0f,0x6f,0x54,0x24,0x30, 0xf3,0x0f,0x6f,0x5c,0x24,0x40,
        0xf3,0x0f,0x6f,0x64,0x24,0x50, 0xf3,0x0f,0x6f,0x6c,0x24,0x60,
        0x48,0x83,0xc4,0x70, 0x41,0x5b, 0x41,0x5a, 0x41,0x59, 0xc3};                                 /* add rsp; pop r11, r10, r9; ret */
    uint64_t target = (uint64_t)(uintptr_t)ErBudget;
    memcpy(c, save, sizeof save); c += sizeof save;
    memcpy(c, &target, 8); c += 8;
    memcpy(c, restore, sizeof restore);
}

/* The stub, and the function's entry jumping to it (jmp [rip+0]; its address: no branch lands in those 14 bytes).
 * NULL, or why not. */
static const char* ErBudgetInstall(uintptr_t game)
{
    if (!PinMatches(game + ER_BUDGET, kErBudgetPin, sizeof kErBudgetPin)) return "the enemy budget's weight lookup is not the reviewed one";
    uint8_t* stub = (uint8_t*)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (stub == NULL) return "no memory for the enemy budget's weight lookup";
    ErBudgetStub(stub);
    DWORD previous;
    VirtualProtect(stub, 0x1000, PAGE_EXECUTE_READ, &previous);
    FlushInstructionCache(GetCurrentProcess(), stub, 0x1000);
    g_erGame = game;
    g_erStub = stub;
    uint8_t jump[14] = {0xff, 0x25, 0, 0, 0, 0};
    uint64_t to = (uint64_t)(uintptr_t)stub;
    memcpy(jump + 6, &to, 8);
    return JournalPut(game + ER_BUDGET, jump, sizeof jump) ? "the enemy budget's weight lookup could not be replaced" : NULL;
}

/* AiEnemyBudget keeps a pair per unit type and spawn source alive: 24 bytes each from +0x1010, their count at +0x1640,
 * a pair gone when its last enemy dies (0xA2D700). Its two appends (0xA2BE29, 0xA2D6A9) check no bound and its lookups
 * reach 64, so a 65th pair is never found again and the 67th write lands on the count. More enemy types alive make
 * more pairs: a full table takes no new pair. That type's alive count is then not kept (its cap, 0x952FF0, sees none). */
#define ER_PAIR_COUNT 0x1640u
static const struct { uint32_t site, size, resume, skip; uint8_t pin[16]; uint8_t skipPin[5]; } kErPairSites[2] = {
    {0xA2BE29u, 7, 0xA2BE30u, 0xA2BE62u, {0x41,0x8b,0x85,0x40,0x16,0x00,0x00,0x4c,0x8d,0x0c,0x40,0x4b,0x89,0x94,0xcd,0x10}, {0x48,0x8b,0x05,0x87,0x10}},
    {0xA2D6A9u, 6, 0xA2D6AFu, 0xA2D6E0u, {0x8b,0x83,0x40,0x16,0x00,0x00,0x4c,0x8d,0x0c,0x40,0x4a,0x89,0x94,0xcb,0x10,0x10}, {0x48,0x8b,0x5c,0x24,0x30}}};
#define ER_PAIR_CAVE 0x40 /* each site's cave in the page */

/* A site's cave: its own load of the count (mov eax, [r13 or rbx + 0x1640]), back to the append below 64, else counted
 * and on to where the game goes without a new pair. rax is dead at both skip targets. */
static void ErPairCave(uint8_t* c, uintptr_t game, int site)
{
    static const uint8_t full[] = {0x83,0xf8,ER_PAIRS, 0x73,0x05}; /* cmp eax, 64; jae full (past the jump back) */
    static const uint8_t count[] = {0xf0,0xff,0x00};               /* lock inc dword [rax] */
    memcpy(c, kErPairSites[site].pin, kErPairSites[site].size); c += kErPairSites[site].size;
    memcpy(c, full, sizeof full); c += sizeof full;
    c = SgJump(c, 0xe9, game + kErPairSites[site].resume);
    c = SgImm64(c, (uint64_t)(uintptr_t)&g_erPairsFull);
    memcpy(c, count, sizeof count); c += sizeof count;
    SgJump(c, 0xe9, game + kErPairSites[site].skip);
}

/* Both appends through their caves (a jump over each site's load, padded with nops). NULL, or why not. */
static const char* ErPairGuardInstall(uintptr_t game)
{
    for (int i = 0; i < 2; i++)
        if (!PinMatches(game + kErPairSites[i].site, kErPairSites[i].pin, 16) || !PinMatches(game + kErPairSites[i].skip, kErPairSites[i].skipPin, 5))
            return "the enemy budget's pair table is not the reviewed one";
    uint8_t* cave = AllocateNearGame(game, 0x1000);
    if (cave == NULL) return "no memory near game.dll for the enemy budget's guard";
    DWORD previous;
    for (int i = 0; i < 2; i++) ErPairCave(cave + i * ER_PAIR_CAVE, game, i);
    VirtualProtect(cave, 0x1000, PAGE_EXECUTE_READ, &previous);
    FlushInstructionCache(GetCurrentProcess(), cave, 0x1000);
    g_erPairCave = cave;
    for (int i = 0; i < 2; i++)
    {
        uint8_t jump[7] = {0xe9, 0, 0, 0, 0, 0x90, 0x90};
        int32_t rel = (int32_t)((intptr_t)(cave + i * ER_PAIR_CAVE) - (intptr_t)(game + kErPairSites[i].site + 5));
        memcpy(jump + 1, &rel, 4);
        if (JournalPut(game + kErPairSites[i].site, jump, kErPairSites[i].size)) return "the enemy budget's pair table could not be guarded";
    }
    return NULL;
}

/* ---- growing a faction's roster -------------------------------------------------------------------------------------- */

static uint64_t ErRowsOf(const uint8_t* rows, uint64_t count, uint64_t unit)
{
    uint64_t found = 0;
    for (uint64_t i = 0; i < count; i++) found += memcmp(rows + i * ER_ROW + 8, &unit, 8) == 0;
    return found;
}

static int ErJoins(const ErEnemy* e, uint32_t f) { return e->state >= 0 && (e->factions == 0 || (e->factions & kErFactions[f].bit)); }

/* The rows an enemy gets in faction f: one of its own, or one per row of its `like`. */
static uint64_t ErRowsFor(const ErEnemy* e, uint32_t f, const uint8_t* rows, uint64_t count)
{
    return !ErJoins(e, f) ? 0 : e->like ? ErRowsOf(rows, count, e->like) : 1;
}

static float ErLevelWeight(const ErEnemy* e, uint32_t level, float weight)
{
    return (e->lowest && level < e->lowest) || (e->highest && level > e->highest) ? 0.0f : weight * e->weight;
}

/* A row of the mod's own: every field the game reads from it. +0x14 stays 0 (its readers ask only for 2, the game's
 * own mark) and +0x1C/+0x28 too (a spawn on death; level 11's weights take that room). */
static void ErOwnRow(uint8_t* row, const ErEnemy* e, int moved)
{
    uint32_t levels = moved ? DIF_VANILLA + 1 : DIF_VANILLA, at = moved ? 0x2C : 0x30;
    memset(row, 0, ER_ROW);
    memcpy(row, &e->key, 4);
    memcpy(row + 8, &e->unit, 8);
    memcpy(row + 0x10, &e->sizeClass, 4);
    memcpy(row + 0x18, &e->cap, 4);
    memcpy(row + 0x20, &e->cost, 4);
    memcpy(row + 0x24, &e->budget, 4);
    for (uint32_t i = 0; i < levels; i++)
    {
        uint32_t level = i < DIF_VANILLA ? i + 1 : DIF_VANILLA;
        float weight = ErLevelWeight(e, level, e->weights[level - 1]);
        memcpy(row + at + 4 * i, &weight, 4);
    }
    memcpy(row + 0x58, e->with, sizeof e->with);
    memcpy(row + 0x68, e->without, sizeof e->without);
    row[0x78] = (e->flags & HF_ENEMY_ALWAYS_LOADED) != 0;
}

/* A copy of `like`'s row for the mod's unit: its weights scaled, 0 outside its levels. Level 11's weight, where the
 * rows have one (moved), counts as level 10's; rows without one get it from level 10's when difficulty.c moves them. */
static void ErRow(uint8_t* row, const uint8_t* like, const ErEnemy* e, int moved)
{
    uint32_t levels = moved ? DIF_VANILLA + 1 : DIF_VANILLA, at = moved ? 0x2C : 0x30;
    memcpy(row, like, ER_ROW);
    memcpy(row + 8, &e->unit, 8);
    for (uint32_t i = 0; i < levels; i++)
    {
        uint32_t level = i < DIF_VANILLA ? i + 1 : DIF_VANILLA;
        float weight;
        memcpy(&weight, row + at + 4 * i, 4);
        weight = ErLevelWeight(e, level, weight);
        memcpy(row + at + 4 * i, &weight, 4);
    }
}

/* The mod enemies' rows after the game's `count` in g->rows (room made by ErGrowFaction); each entry's rows counted.
 * g->names gets each row's group name: 0x94B570 and 0xAD51B0 name a group by the index of its first row. */
static void ErAppendRow(ErGrown* g, uint64_t count, int entry, const char* name)
{
    g->names[g->rowCount] = name;
    g->rowOwner[g->rowCount - count] = entry;
    g->rowCount++;
    ErEnemyAt(entry)->rows++;
}

static void ErAppendRows(ErGrown* g, uint32_t f, uint64_t count, int moved)
{
    for (int i = 0; i < g_erEnemyCount; i++)
    {
        ErEnemy* e = ErEnemyAt(i);
        if (!ErJoins(e, f)) continue;
        if (e->like == 0)
        {
            ErOwnRow(g->rows + g->rowCount * ER_ROW, e, moved);
            ErAppendRow(g, count, i, e->group);
        }
        for (uint64_t r = 0; e->like != 0 && r < count; r++)
        {
            const uint8_t* like = g->rows + r * ER_ROW;
            if (memcmp(like + 8, &e->like, 8) != 0) continue;
            ErRow(g->rows + g->rowCount * ER_ROW, like, e, moved);
            ErAppendRow(g, count, i, g->names[r]);
        }
    }
}

/* Whether a rule applies in faction f: one of the factions it names (any, with none) whose roster has a row of its unit.
 * A unit of a shared roster only is left out: the package loader resolves those rows with no faction's rules. */
static int ErSwapIn(const ErSwap* s, uint32_t f, const ErGrown* g)
{
    if (s->state < 0 || (s->factions && !(s->factions & kErFactions[f].bit))) return 0;
    return ErRowsOf(g->rows, g->rowCount, s->unit) > 0;
}

/* Whether `left` replacements in a row can start at `unit`, whatever the rules' tags and levels. As 0x949B80 chains
 * them: a rule follows another only with a priority not below it (`floor`; the game's Illuminate rules loop but for that). */
static int ErChains(const uint8_t* rules, uint64_t count, uint64_t unit, uint32_t left, uint32_t floor)
{
    if (left == 0) return 1;
    for (uint64_t i = 0; i < count; i++)
    {
        const uint8_t* rule = rules + i * ER_RULE;
        uint64_t next;
        uint32_t priority;
        memcpy(&next, rule + 0x10, 8);
        memcpy(&priority, rule + 0x28, 4);
        if (memcmp(rule + 8, &unit, 8) == 0 && priority >= floor && ErChains(rules, count, next, left - 1, priority)) return 1;
    }
    return 0;
}

/* Whether the rules hold a chain longer than the game takes (a loop of rules is one). */
static int ErChainTooLong(const uint8_t* rules, uint64_t count)
{
    for (uint64_t i = 0; i < count; i++)
    {
        uint64_t unit;
        memcpy(&unit, rules + i * ER_RULE + 8, 8);
        if (ErChains(rules, count, unit, ER_CHAIN + 1, 0)) return 1;
    }
    return 0;
}

/* A rule as the game's: 0x949B80 reads +0, +8, +0x10 and +0x20..+0x28, the budget +0x1C, and 0x949FBD +0x18, a factor
 * on the replaced row's cost (+0x20; 0.5 to 1.2 in the game's). Its budget weight defaults to the replaced row's. */
static void ErRule(uint8_t* rule, const ErSwap* s, const ErGrown* g)
{
    float cost = s->cost > 0 ? s->cost : 1.0f, budget = s->budget;
    uint32_t lowest = s->lowest ? s->lowest : 1, highest = s->highest ? s->highest : 100;
    for (uint64_t i = 0; budget <= 0 && i < g->rowCount; i++)
        if (memcmp(g->rows + i * ER_ROW + 8, &s->unit, 8) == 0) memcpy(&budget, g->rows + i * ER_ROW + 0x24, 4);
    if (budget <= 0) budget = 1.0f;
    memset(rule, 0, ER_RULE);
    memcpy(rule, &s->tag, 4);
    memcpy(rule + 8, &s->unit, 8);
    memcpy(rule + 0x10, &s->replacement, 8);
    memcpy(rule + 0x18, &cost, 4);
    memcpy(rule + 0x1C, &budget, 4);
    memcpy(rule + 0x20, &lowest, 4);
    memcpy(rule + 0x24, &highest, 4);
    memcpy(rule + 0x28, &s->priority, 4);
}

/* The mods' rules after the game's `count` in g->rules. A rule that would make a chain too long is left out. */
static void ErAppendRules(ErGrown* g, uint32_t f, uint64_t count)
{
    for (int i = 0; i < g_erSwapCount; i++)
    {
        ErSwap* s = ErSwapAt(i);
        if (!ErSwapIn(s, f, g)) continue;
        ErRule(g->rules + g->ruleCount * ER_RULE, s, g);
        if (ErChainTooLong(g->rules, g->ruleCount + 1))
        {
            ApiLog(s->mod, "add_enemy_swap(%s): with the other rules it would replace over %u times in a row, or in a loop; left out of the %s",
                s->name, ER_CHAIN, f == 0 ? "Terminids" : f == 1 ? "Automatons" : "Illuminate");
            continue;
        }
        g->ruleOwner[g->ruleCount - count] = i;
        g->ruleCount++;
        s->rules++;
    }
}

/* Faction f's pairs as reviewed, not grown yet: NULL with its rows and rules, or why not. */
static const char* ErVanilla(uintptr_t game, uint32_t f, const uint8_t** rows, const uint8_t** rules)
{
    uint64_t pair[4];
    if (SafeRead(game + kRosters[kErFactions[f].roster].descriptor, pair, sizeof pair)) return "an enemy roster is unreadable";
    if (pair[0] == 0 || pair[1] != kRosters[kErFactions[f].roster].rows || pair[2] == 0 || pair[3] != kErFactions[f].rules)
        return "an enemy roster is not the reviewed one";
    *rows = (const uint8_t*)(uintptr_t)pair[0];
    *rules = (const uint8_t*)(uintptr_t)pair[2];
    return NULL;
}

/* Room for a faction's copies: the game's rows and rules, and at most one row per game row for each enemy that joins
 * and one rule per rule added. Zeroed pages. 0 on success. */
static int ErRoom(ErGrown* g, uint32_t f, uint64_t rows, uint64_t rules)
{
    uint64_t joining = 0;
    for (int i = 0; i < g_erEnemyCount; i++) joining += ErRowsFor(ErEnemyAt(i), f, g->rows, rows);
    if (rows + joining > ER_ROWS_MAX) return 1;
    size_t rowBytes = (size_t)(rows + joining) * ER_ROW, ruleBytes = (size_t)(rules + (uint64_t)g_erSwapCount) * ER_RULE;
    size_t ownerBytes = (size_t)(joining + (uint64_t)g_erSwapCount + 1) * sizeof(int);
    uint8_t* block = (uint8_t*)VirtualAlloc(NULL, rowBytes + ruleBytes + 2 * ownerBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (block == NULL) return 1;
    memcpy(block, g->rows, (size_t)rows * ER_ROW);
    memcpy(block + rowBytes, g->rules, (size_t)rules * ER_RULE);
    const char** names = (const char**)(void*)AllocateNearGame(g_erGame, (size_t)(rows + joining) * 8);
    if (names == NULL || SafeRead(g_erGame + kErNames[f], names, (size_t)rows * 8)) { VirtualFree(block, 0, MEM_RELEASE); return 1; }
    g->names = names;
    g->rows = block;
    g->rules = block + rowBytes;
    g->rowOwner = (int*)(void*)(block + rowBytes + ruleBytes);
    g->ruleOwner = (int*)(void*)(block + rowBytes + ruleBytes + ownerBytes);
    return 0;
}

/* Faction f's copies with the mods' rows and rules, then its pairs: a pointer before its count, so a reader between
 * the two writes sees the copy to the game's count. NULL, or why not (nothing of this faction changed then). */
static const char* ErGrowFaction(uintptr_t game, uint32_t f, int moved)
{
    ErGrown* g = &g_erGrown[f];
    const uint8_t* rows = NULL;
    const uint8_t* rules = NULL;
    const char* why = ErVanilla(game, f, &rows, &rules);
    if (why != NULL) return why;
    uint64_t rowCount = kRosters[kErFactions[f].roster].rows, ruleCount = kErFactions[f].rules;
    g->rows = (uint8_t*)(uintptr_t)rows; g->rules = (uint8_t*)(uintptr_t)rules;
    if (ErRoom(g, f, rowCount, ruleCount)) { memset(g, 0, sizeof *g); return "no memory for a larger enemy roster, or over 65535 rows"; }
    g->rowCount = rowCount; g->ruleCount = ruleCount;
    ErAppendRows(g, f, rowCount, moved);
    ErAppendRules(g, f, ruleCount);
    MemoryBarrier(); /* the copies are whole before a pair shows them */
    uintptr_t d = game + kRosters[kErFactions[f].roster].descriptor;
    uint64_t at[2] = {(uint64_t)(uintptr_t)g->rows, (uint64_t)(uintptr_t)g->rules};
    if (ApiWrite(d, &at[0], 8) || ApiWrite(d + 8, &g->rowCount, 8) || ApiWrite(d + 0x10, &at[1], 8) || ApiWrite(d + 0x18, &g->ruleCount, 8))
        return "an enemy roster's rows could not be replaced";
    return NULL;
}

/* The packages the enemies name, asked for once the entries are known to stay: resident for the session, as a mod's
 * load_package leaves them. An enemy whose package fails stays out: its spawn would read a unit that is not loaded. */
static int ApiLoadPackage(HfMod* mod, uint64_t package); /* packages.c */
static int (*g_erWantPackage)(HfMod* mod, uint64_t package) = ApiLoadPackage;
static void ErWantPackages(void)
{
    for (int i = 0; i < g_erEnemyCount; i++)
    {
        ErEnemy* e = ErEnemyAt(i);
        for (int k = 0; e->state >= 0 && k < 2; k++)
        {
            if (e->packages[k] == 0 || g_erWantPackage(e->mod, e->packages[k])) continue;
            e->state = -1;
            ApiLog(e->mod, "add_enemy(%s): its package %016llx could not be loaded; not added", e->name, (unsigned long long)e->packages[k]);
        }
    }
}

/* Entries whose entities the game does not have are refused before any row is made: the director would spawn them. */
static void ErCheckEntities(void)
{
    for (int i = 0; i < g_erEnemyCount; i++)
    {
        ErEnemy* e = ErEnemyAt(i);
        if (ModGone(e->mod) || !e->mod->loaded) e->state = -1;
        else if (!g_erEntityKnown(e->unit))
        { e->state = -1; ApiLog(e->mod, "add_enemy(%s): the game has no entity %016llx; not added", e->name, (unsigned long long)e->unit); }
    }
    for (int i = 0; i < g_erSwapCount; i++)
    {
        ErSwap* s = ErSwapAt(i);
        if (ModGone(s->mod) || !s->mod->loaded) s->state = -1;
        else if (!g_erEntityKnown(s->unit) || !g_erEntityKnown(s->replacement))
        { s->state = -1; ApiLog(s->mod, "add_enemy_swap(%s): the game has not both its entities; not added", s->name); }
    }
}

/* What each enemy got, in its mod's log. */
static void ErReportEnemies(void)
{
    for (int i = 0; i < g_erEnemyCount; i++)
    {
        ErEnemy* e = ErEnemyAt(i);
        if (e->state < 0) continue;
        e->state = e->rows > 0 ? 1 : -1;
        if (e->rows > 0) ApiLog(e->mod, "enemy %s: %d roster row(s)", e->name, e->rows);
        else ApiLog(e->mod, "add_enemy(%s): %016llx has no row in its faction rosters; not added", e->name, (unsigned long long)e->like);
    }
}

static void ErReportSwaps(void)
{
    for (int i = 0; i < g_erSwapCount; i++)
    {
        ErSwap* s = ErSwapAt(i);
        if (s->state < 0) continue;
        s->state = s->rules > 0 ? 1 : -1;
        if (s->rules > 0) ApiLog(s->mod, "enemy rule %s: in %d roster(s)", s->name, s->rules);
        else ApiLog(s->mod, "add_enemy_swap(%s): no rule made (its factions' rosters need a row of %016llx); not added", s->name, (unsigned long long)s->unit);
    }
}

/* Every faction roster grown, once: NULL, or why not. A faction that fails keeps the game's rows. */
static const char* ErGrow(void)
{
    size_t size = 0;
    uintptr_t game = g_directorImage(&size);
    if (game == 0 || size != DIR_IMAGE || strcmp(g_build, "25480438")) return "unsupported game build";
    int moved = DirectorWeightsMoved(game);
    if (moved < 0) return "an enemy weight read is not the reviewed one";
    const char* why = ErPairGuardInstall(game);
    if (why == NULL) why = ErBudgetInstall(game);
    if (why == NULL) why = EnemyGuardsInstall(game);
    if (why != NULL) return why;
    ErCheckEntities();
    ErWantPackages();
    for (uint32_t f = 0; f < ER_FACTIONS; f++)
    {
        const char* failed = ErGrowFaction(game, f, moved);
        if (failed == NULL) continue;
        why = failed;
        g_erGrown[f].rowCount = g_erGrown[f].ruleCount = 0; /* not counted as added */
        Note("enemies: roster %u not grown: %s", kErFactions[f].roster, failed);
    }
    if (why == NULL) why = EnemyNamesInstall(game);
    if (why == NULL) why = EnemySquadsGrow(game, moved);
    ErReportEnemies();
    ErReportSwaps();
    return why;
}

/* Entries no row or rule was tried for (the growth stopped before them): why, in their mods' logs. */
static void ErTellPending(const char* why)
{
    for (int i = 0; why != NULL && i < g_erEnemyCount; i++)
        if (ErEnemyAt(i)->state == 0) { ErEnemyAt(i)->state = -1; ApiLog(ErEnemyAt(i)->mod, "add_enemy(%s): not added: %s", ErEnemyAt(i)->name, why); }
    for (int i = 0; why != NULL && i < g_erSwapCount; i++)
        if (ErSwapAt(i)->state == 0) { ErSwapAt(i)->state = -1; ApiLog(ErSwapAt(i)->mod, "add_enemy_swap(%s): not added: %s", ErSwapAt(i)->name, why); }
    EnemySquadsTell(why);
}

/* What the pair guard caught, at most every 10 s. */
static void ErPairsWatch(void)
{
    LONG full = g_erPairsFull;
    if (full == g_erPairsFullReported || GetTickCount64() - g_erPairsReportedAt < 10000) return;
    g_erPairsReportedAt = GetTickCount64();
    g_erPairsFullReported = full;
    Note("enemies: the enemy budget's table was full (%u kinds of enemy and spawn source alive) %ld time(s) so far; those spawns are not counted in it", ER_PAIRS, (long)full);
}

/* The watch loop: the rosters grow once the entity data loaded (the entities are checked against it), before any
 * mission reads them. */
static void WatchEnemyRosters(void)
{
    if (g_erState == 1) { ErPairsWatch(); ErForgetGone(); EnemySquadsWatch(); EnemyGuardsWatch(); }
    if (!EnemyRostersPending() || !g_fired[HF_EVENT_GAME_LOADED]) return;
    if (g_erSince == 0) g_erSince = GetTickCount64();
    if (!g_fired[HF_EVENT_ENTITIES_READY])
    {
        if (GetTickCount64() - g_erSince < ER_WAIT_MS) return;
        g_erState = -1;
        Note("enemies: not added; the entity data did not load");
        ErTellPending("the entity data did not load");
        return;
    }
    const char* why = ErGrow();
    g_erState = why == NULL ? 1 : -1;
    ErTellPending(why);
    uint64_t rows = 0, rules = 0;
    for (uint32_t f = 0; f < ER_FACTIONS; f++)
    {
        rows += g_erGrown[f].rowCount ? g_erGrown[f].rowCount - kRosters[kErFactions[f].roster].rows : 0;
        rules += g_erGrown[f].ruleCount ? g_erGrown[f].ruleCount - kErFactions[f].rules : 0;
    }
    Note("enemies: %llu roster row(s), %llu replacement rule(s) and %llu squad(s) added%s%s", (unsigned long long)rows, (unsigned long long)rules,
        (unsigned long long)EnemySquadsAdded(), why ? "; not all: " : "", why ? why : "");
}

/* Whether a mission runs: the director's +0x680 is 1 from its start to the return to the ship. */
static int ErMissionRuns(void)
{
    uint64_t director = 0;
    uint32_t running = 0;
    if (g_erGame == 0 || SafeRead(g_erGame + ER_DIRECTOR, &director, 8) || director == 0) return 0;
    return !SafeRead((uintptr_t)director + 0x680, &running, 4) && running != 0;
}

/* The rows and rules of mods taken out lose their weights and never match. Not while a mission runs: its plan holds
 * squads that draw those groups, and a group with no row left to draw crashes the game (0x949BF9). */
static void ErForgetGone(void)
{
    if (!g_erForgetting || ErMissionRuns()) return;
    g_erForgetting = 0;
    for (uint32_t f = 0; f < ER_FACTIONS; f++)
    {
        ErGrown* g = &g_erGrown[f];
        if (g->rowOwner == NULL) continue;
        uint64_t rows = kRosters[kErFactions[f].roster].rows, rules = kErFactions[f].rules;
        for (uint64_t r = rows; r < g->rowCount; r++)
            if (ErEnemyAt(g->rowOwner[r - rows])->gone) memset(g->rows + r * ER_ROW + 0x2C, 0, 0x2C);
        uint32_t never = ER_NEVER;
        for (uint64_t r = rules; r < g->ruleCount; r++)
            if (ErSwapAt(g->ruleOwner[r - rules])->gone) memcpy(g->rules + r * ER_RULE + 0x20, &never, 4);
    }
    EnemySquadsForget();
}

/* A mod taken out (live_code.c): its entries are marked, and dropped from the rosters once no mission runs. Its
 * entities stay, as tombstones, so a row the director already picked still spawns. */
static void EnemyRostersForget(const HfMod* mod)
{
    for (int i = 0; i < g_erEnemyCount; i++) if (ErEnemyAt(i)->mod == mod) ErEnemyAt(i)->gone = 1;
    for (int i = 0; i < g_erSwapCount; i++) if (ErSwapAt(i)->mod == mod) ErSwapAt(i)->gone = 1;
    EnemySquadsMark(mod);
    g_erForgetting = 1;
    ErForgetGone();
}
