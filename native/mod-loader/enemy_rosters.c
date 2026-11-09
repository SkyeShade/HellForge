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
static const uint8_t kErBudgetPin[] = {0x48, 0x8b, 0x05, 0xc9, 0xb2, 0x9d, 0x02, 0x8b, 0x88, 0xbc, 0x18, 0x05, 0x00, 0x33, 0xc0, 0x83,
    0xf9, 0x02, 0x0f, 0x85, 0xbe, 0x00, 0x00, 0x00, 0x8b, 0xc8};

/* state: 0 waiting for the rosters to grow, 1 in them (rows: how many it got), -1 refused then. */
typedef struct ErEnemy { HfMod* mod; char name[192]; uint64_t unit, like; float weight; uint32_t factions, lowest, highest; int state, rows; } ErEnemy;
typedef struct ErSwap
{
    HfMod* mod;
    char name[192];
    uint64_t unit, replacement;
    uint32_t factions, tag, lowest, highest, priority;
    float budget, cost;
    int state, rules;
} ErSwap;
/* A faction's grown arrays; the mod entry (its index) behind each row and rule after the game's. */
typedef struct ErGrown { uint8_t* rows; uint8_t* rules; uint64_t rowCount, ruleCount; int* rowOwner; int* ruleOwner; } ErGrown;

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
static int EnemyRostersPending(void) { return g_erState == 0 && (g_erEnemyCount > 0 || g_erSwapCount > 0); }

/* Whether the game has this entity: in its entity map, and not one of a mod's that was refused. */
static int ErEntityInMap(uint64_t entity)
{
    uint8_t* map = StatePointer(EntityState(), REG_MAP);
    return map != NULL && FindRow(map, REG_MAP_SLOTS, 32, entity) != NULL && !RefusedEntity(entity);
}
/* The native fixture replaces it. */
static int (*g_erEntityKnown)(uint64_t entity) = ErEntityInMap;

/* An entity's SquadTransportFormation record (component 122: two entries of 24 bytes, a formation id first), or NULL.
 * A transport looks each passenger's up (0x4FE170) and reads its formation's entry with no check (0x684A70, 0x684B51):
 * an enemy drawn for a transport without that entry crashes the game. */
#define ER_FORMATIONS 122u
static const uint8_t* ErFormationsOf(uint64_t entity)
{
    uint8_t* table = StatePointer(EntityState(), REG_TABLES + 8 * ER_FORMATIONS);
    uint32_t slots = SlotsOf(ER_FORMATIONS), record = 0;
    uint8_t* row = table != NULL && slots != 0 ? FindRow(table, slots, 16, entity) : NULL;
    if (row == NULL) return NULL;
    memcpy(&record, row + 8, 4);
    return table + (size_t)slots * 16 + (size_t)record * 48;
}
static const uint8_t* (*g_erFormations)(uint64_t entity) = ErFormationsOf;

/* Whether `unit` can ride every transport `as` can: each formation of `as`'s record is in `unit`'s. */
static int ErRidesAs(uint64_t unit, uint64_t as)
{
    const uint8_t* need = g_erFormations(as);
    const uint8_t* have = g_erFormations(unit);
    if (need == NULL || need == have) return 1;
    for (int n = 0; n < 2; n++)
    {
        uint32_t id, found = 0;
        memcpy(&id, need + n * 24, 4);
        for (int h = 0; have != NULL && h < 2; h++) found |= memcmp(have + h * 24, &id, 4) == 0;
        if (id != 0 && !found) return 0;
    }
    return 1;
}

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

/* add_enemy (SDK 17): kept until the entity data loads, when the rosters grow (ErGrow). */
static int ApiAddEnemy(HfMod* mod, const HfEnemy* def)
{
    if (mod == NULL || def == NULL || def->size < offsetof(HfEnemy, highest_level) + sizeof def->highest_level)
    { ApiLog(mod, "add_enemy: no enemy, or its size is not set (HfEnemy e = {sizeof e})"); return 1; }
    char identity[256];
    const char* name = Identity(mod, def->name, identity, sizeof identity);
    if (ErRefused(mod, "add_enemy", name, def->factions) ||
        ErLevelsRefused(mod, "add_enemy", name, def->lowest_level, def->highest_level, DIF_VANILLA)) return 1;
    if (def->unit == 0 || def->like == 0) { ApiLog(mod, "add_enemy(%s): it needs a unit and the enemy it is like", name); return 1; }
    if (!isfinite(def->weight) || def->weight < 0 || def->weight > 1000) { ApiLog(mod, "add_enemy(%s): its weight is 0 to 1000", name); return 1; }
    if (!StableRoom(&g_erEnemyStore, (size_t)g_erEnemyCount + 1)) { ApiLog(mod, "add_enemy(%s): no memory for more enemies", name); return 1; }
    ErEnemy e = {0};
    e.mod = mod;
    snprintf(e.name, sizeof e.name, "%s", name);
    e.unit = def->unit; e.like = def->like;
    e.weight = def->weight > 0 ? def->weight : 1.0f;
    e.factions = def->factions; e.lowest = def->lowest_level; e.highest = def->highest_level;
    *ErEnemyAt(g_erEnemyCount) = e;
    g_erEnemyCount++;
    ApiLog(mod, "adding enemy %s: %016llx beside %016llx, its weights x %g", name, (unsigned long long)e.unit, (unsigned long long)e.like, e.weight);
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
        weight = (e->lowest && level < e->lowest) || (e->highest && level > e->highest) ? 0.0f : weight * e->weight;
        memcpy(row + at + 4 * i, &weight, 4);
    }
}

/* The mod enemies' rows after the game's `count` in g->rows (room made by ErGrowFaction); each entry's rows counted. */
static void ErAppendRows(ErGrown* g, uint32_t f, uint64_t count, int moved)
{
    for (int i = 0; i < g_erEnemyCount; i++)
    {
        ErEnemy* e = ErEnemyAt(i);
        for (uint64_t r = 0; ErJoins(e, f) && r < count; r++)
        {
            const uint8_t* like = g->rows + r * ER_ROW;
            if (memcmp(like + 8, &e->like, 8) != 0) continue;
            ErRow(g->rows + g->rowCount * ER_ROW, like, e, moved);
            g->rowOwner[g->rowCount - count] = i;
            g->rowCount++;
            e->rows++;
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
    for (int i = 0; i < g_erEnemyCount; i++) joining += ErJoins(ErEnemyAt(i), f) ? ErRowsOf(g->rows, rows, ErEnemyAt(i)->like) : 0;
    if (rows + joining > ER_ROWS_MAX) return 1;
    size_t rowBytes = (size_t)(rows + joining) * ER_ROW, ruleBytes = (size_t)(rules + (uint64_t)g_erSwapCount) * ER_RULE;
    size_t ownerBytes = (size_t)(joining + (uint64_t)g_erSwapCount + 1) * sizeof(int);
    uint8_t* block = (uint8_t*)VirtualAlloc(NULL, rowBytes + ruleBytes + 2 * ownerBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (block == NULL) return 1;
    memcpy(block, g->rows, (size_t)rows * ER_ROW);
    memcpy(block + rowBytes, g->rules, (size_t)rules * ER_RULE);
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

/* Whether an entity has a Behavior record (component 284). The director reads an enemy's with no check (0x510790, then
 * 0x90EC29 and 0x90F069): a prop or weapon drawn as an enemy crashes the game. */
#define ER_BEHAVIOR 284u
static int ErBehaviorOf(uint64_t entity)
{
    uint8_t* table = StatePointer(EntityState(), REG_TABLES + 8 * ER_BEHAVIOR);
    uint32_t slots = SlotsOf(ER_BEHAVIOR);
    return table != NULL && slots != 0 && FindRow(table, slots, 16, entity) != NULL;
}
static int (*g_erBehaves)(uint64_t entity) = ErBehaviorOf;

/* Why `unit` cannot stand where `as` does, or NULL: it is an AI unit when `as` is one, and rides its transports. */
static const char* ErStandsFor(uint64_t unit, uint64_t as)
{
    if (g_erBehaves(as) && !g_erBehaves(unit)) return "it has no Behavior record, so it is not an AI unit";
    if (!ErRidesAs(unit, as)) return "it has no SquadTransportFormation for the transports that carry that enemy";
    return NULL;
}

/* Entries whose entities the game does not have are refused before any row is made: the director would spawn them. */
static void ErCheckEntities(void)
{
    const char* why;
    for (int i = 0; i < g_erEnemyCount; i++)
    {
        ErEnemy* e = ErEnemyAt(i);
        if (ModGone(e->mod) || !e->mod->loaded) e->state = -1;
        else if (!g_erEntityKnown(e->unit))
        { e->state = -1; ApiLog(e->mod, "add_enemy(%s): the game has no entity %016llx; not added", e->name, (unsigned long long)e->unit); }
        else if ((why = ErStandsFor(e->unit, e->like)) != NULL)
        { e->state = -1; ApiLog(e->mod, "add_enemy(%s): the unit cannot stand for %016llx: %s; not added (base the unit on that enemy)", e->name, (unsigned long long)e->like, why); }
    }
    for (int i = 0; i < g_erSwapCount; i++)
    {
        ErSwap* s = ErSwapAt(i);
        if (ModGone(s->mod) || !s->mod->loaded) s->state = -1;
        else if (!g_erEntityKnown(s->unit) || !g_erEntityKnown(s->replacement))
        { s->state = -1; ApiLog(s->mod, "add_enemy_swap(%s): the game has not both its entities; not added", s->name); }
        else if ((why = ErStandsFor(s->replacement, s->unit)) != NULL)
        { s->state = -1; ApiLog(s->mod, "add_enemy_swap(%s): the replacement cannot stand for %016llx: %s; not added", s->name, (unsigned long long)s->unit, why); }
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
    if (why != NULL) return why;
    ErCheckEntities();
    for (uint32_t f = 0; f < ER_FACTIONS; f++)
    {
        const char* failed = ErGrowFaction(game, f, moved);
        if (failed == NULL) continue;
        why = failed;
        g_erGrown[f].rowCount = g_erGrown[f].ruleCount = 0; /* not counted as added */
        Note("enemies: roster %u not grown: %s", kErFactions[f].roster, failed);
    }
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
    if (g_erState == 1) ErPairsWatch();
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
    Note("enemies: %llu roster row(s) and %llu replacement rule(s) added%s%s", (unsigned long long)rows, (unsigned long long)rules,
        why ? "; not all: " : "", why ? why : "");
}

/* A mod taken out (live_code.c): its rows lose their weights and its rules never match. Its entities stay, as
 * tombstones, so a row the director already picked still spawns. */
static void EnemyRostersForget(const HfMod* mod)
{
    for (uint32_t f = 0; f < ER_FACTIONS; f++)
    {
        ErGrown* g = &g_erGrown[f];
        if (g->rowOwner == NULL) continue;
        uint64_t rows = kRosters[kErFactions[f].roster].rows, rules = kErFactions[f].rules;
        for (uint64_t r = rows; r < g->rowCount; r++)
            if (ErEnemyAt(g->rowOwner[r - rows])->mod == mod) memset(g->rows + r * ER_ROW + 0x2C, 0, 0x2C);
        uint32_t never = ER_NEVER;
        for (uint64_t r = rules; r < g->ruleCount; r++)
            if (ErSwapAt(g->ruleOwner[r - rules])->mod == mod) memcpy(g->rules + r * ER_RULE + 0x20, &never, 4);
    }
}
