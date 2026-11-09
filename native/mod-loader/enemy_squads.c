/* Squads mods add to the spawn director (SDK 17: add_enemy_squad; docs/modding/enemies.md). Included by director.c after
 * enemy_guards.c.
 *
 * A roster descriptor holds eight families of squads (compositions, 0x118 bytes a row) as {rows, count} from +0x20,
 * one per kind of squad. The game reaches a faction's families only through those pairs, so a family gets a larger
 * copy, the game's rows first, as the unit rows do (enemy_rosters.c). The copies are never freed.
 *
 * When a mission starts, 0x94BE10 writes each family's fitting rows into an array of the director's plan with no
 * bound; the arrays hold 8 to 96 entries, and the patrols' and convoys' readers copy them to the stack. A family with
 * more rows than its array holds is selected into a scratch array, and the plan gets as many as fit (EsSelect). */

#define ES_ROW 0x118u
#define ES_ENTRY 0xD8u
#define ES_SELECT 0x94BE10u
#define ES_SQUAD_ENTITY 0xF5252BE8F520F3D8ull /* +0x100 of every game squad but an outpost's (0) */
#define ES_PATROL_KIND 0xCD121F4Bu            /* +0x110 */
#define ES_ID 0x46514C5Du                     /* +0xFC of most game squads; an outpost's is its group's key */
#define ES_MEMBERS_MOST 32u                   /* a squad's enemies, as director.c holds the game's rows to */
#define ES_POOL 999u                          /* an outpost's count: a pool to draw from */

/* The kinds: the family's place in the descriptor, the game's rows per faction (as kErFactions), and what the plan's
 * array of that family holds, told apart by the distance from the array to its count (a constant of the plan). */
static const struct { uint32_t kind, family, rows[ER_FACTIONS], span, cap; const char* name; } kEsKinds[] = {
    {HF_SQUAD_PATROL, 0, {39, 44, 79}, 0x17344u, 96, "patrol"},     {HF_SQUAD_OUTPOST, 2, {27, 32, 17}, 0x1074Cu, 32, "outpost"},
    {HF_SQUAD_ENCOUNTER, 3, {50, 50, 51}, 0xE594u, 64, "encounter"}, {HF_SQUAD_BORDER, 4, {6, 4, 1}, 0xEC50u, 8, "border"},
    {HF_SQUAD_CONVOY, 6, {1, 5, 1}, 0x949Cu, 8, "convoy"},           {HF_SQUAD_AIR_SUPPORT, 7, {1, 1, 3}, 0x8DE0u, 8, "air support"}};
#define ES_KINDS (sizeof kEsKinds / sizeof kEsKinds[0])
static const uint8_t kEsSelectPin[] = {0x40,0x57,0x41,0x55,0x48,0x81,0xec,0x08,0x01,0x00,0x00,0x48,0x8b,0x05,0xee,0x01};

typedef struct EsSquad
{
    HfMod* mod;
    char name[192];
    uint32_t factions, kind, lowest, highest, lowestPlayers, highestPlayers, keys[HF_SQUAD_MEMBERS], counts[HF_SQUAD_MEMBERS], tags[8];
    float weight, cost;
    int state, rows, gone;
} EsSquad;
typedef struct EsGrown { uint8_t* rows; uint64_t count, game; int* owner; } EsGrown;
typedef uint64_t (*EsSelectFn)(void* a, void* roster, const uint8_t* rows, uint32_t count, uint8_t* out, uint32_t* outCount, uint64_t level,
    uint64_t players, void* tags, uint64_t flagA, uint64_t flagB);

static StableArray g_esStore = {NULL, sizeof(EsSquad), ER_CEILING, 0};
static EsGrown g_esGrown[ER_FACTIONS][ES_KINDS];
static void* g_esSelectOriginal;
static uint8_t* g_esScratch;
static uint32_t g_esScratchRows;
static SRWLOCK g_esLock = SRWLOCK_INIT;
static volatile LONG g_esLeftOut;
static LONG g_esLeftOutReported;
static ULONGLONG g_esReportedAt;

static EsSquad* EsAt(int i) { return (EsSquad*)(g_esStore.base + (size_t)i * sizeof(EsSquad)); }
static int EsKind(uint32_t kind)
{
    for (int k = 0; k < (int)ES_KINDS; k++) if (kEsKinds[k].kind == kind) return k;
    return -1;
}

/* A squad's members into s: 1 when refused. */
static int EsMembersRefused(HfMod* mod, const HfEnemySquad* def, const char* name, EsSquad* s)
{
    uint32_t members = 0, total = 0;
    for (int i = 0; i < HF_SQUAD_MEMBERS; i++)
    {
        uint32_t key = ErHash(def->members[i].group);
        if (key == 0) continue;
        if (def->members[i].count == 0 || def->members[i].count > ES_POOL) { ApiLog(mod, "add_enemy_squad(%s): a member's count is 1 to 999", name); return 1; }
        s->keys[members] = key;
        s->counts[members++] = def->members[i].count;
        total += def->members[i].count;
    }
    if (members == 0) { ApiLog(mod, "add_enemy_squad(%s): it needs a member (a group and its count)", name); return 1; }
    if (def->kind != HF_SQUAD_OUTPOST && total > ES_MEMBERS_MOST) { ApiLog(mod, "add_enemy_squad(%s): it holds 32 enemies at most (an outpost's any)", name); return 1; }
    return 0;
}

/* What a squad must have besides its members: 1 when refused. */
static int EsRefused(HfMod* mod, const HfEnemySquad* def, const char* name)
{
    if (ErRefused(mod, "add_enemy_squad", name, def->factions) ||
        ErLevelsRefused(mod, "add_enemy_squad", name, def->lowest_level, def->highest_level, DIF_VANILLA) ||
        ErLevelsRefused(mod, "add_enemy_squad", name, def->lowest_players, def->highest_players, 4)) return 1;
    if (def->factions == 0 || EsKind(def->kind) < 0) { ApiLog(mod, "add_enemy_squad(%s): it needs its factions and a kind (HF_SQUAD_...)", name); return 1; }
    if (!isfinite(def->weight) || def->weight <= 0 || def->weight > 1000 || !isfinite(def->cost) || def->cost < 0 || def->cost > 100)
    { ApiLog(mod, "add_enemy_squad(%s): its weight is above 0, to 1000, and its cost 0 to 100", name); return 1; }
    for (int i = 0; i < g_erSquadCount; i++)
        if (!strcmp(EsAt(i)->name, name)) { ApiLog(mod, "add_enemy_squad(%s): already added by %s", name, EsAt(i)->mod->id); return 1; }
    return 0;
}

/* add_enemy_squad (SDK 17): kept until the rosters grow. */
static int ApiAddEnemySquad(HfMod* mod, const HfEnemySquad* def)
{
    if (mod == NULL || def == NULL || def->size < sizeof(HfEnemySquad)) { ApiLog(mod, "add_enemy_squad: no squad, or its size is not set (HfEnemySquad s = {sizeof s})"); return 1; }
    char identity[256];
    const char* name = Identity(mod, def->name, identity, sizeof identity);
    EsSquad s = {0};
    if (EsRefused(mod, def, name) || EsMembersRefused(mod, def, name, &s)) return 1;
    if (!StableRoom(&g_esStore, (size_t)g_erSquadCount + 1)) { ApiLog(mod, "add_enemy_squad(%s): no memory for more squads", name); return 1; }
    s.mod = mod;
    snprintf(s.name, sizeof s.name, "%s", name);
    s.factions = def->factions; s.kind = def->kind;
    s.lowest = def->lowest_level ? def->lowest_level : 1; s.highest = def->highest_level ? def->highest_level : DIF_VANILLA;
    s.lowestPlayers = def->lowest_players ? def->lowest_players : 1; s.highestPlayers = def->highest_players ? def->highest_players : 4;
    s.weight = def->weight; s.cost = def->cost > 0 ? def->cost : 1.0f;
    for (int i = 0, n = 0; i < 8; i++) if (ErHash(def->with_tags[i])) s.tags[n++] = ErHash(def->with_tags[i]);
    *EsAt(g_erSquadCount) = s;
    g_erSquadCount++;
    ApiLog(mod, "adding enemy squad %s: %s", name, kEsKinds[EsKind(s.kind)].name);
    return 0;
}

/* ---- the plan's arrays ---------------------------------------------------------------------------------------------- */

/* The entries kept when more fit a mission than its array holds: every `got`-th left out evenly, so the game's squads
 * and the mods' lose alike, and the same ones each time the same squads fit (the package loader plans too). */
static uint32_t EsKeep(uint8_t* out, const uint8_t* scratch, uint32_t got, uint32_t cap)
{
    uint32_t kept = 0;
    for (uint32_t i = 0; i < got && kept < cap; i++)
        if ((uint64_t)(i + 1) * cap / got != (uint64_t)i * cap / got) memcpy(out + (size_t)kept++ * ES_ENTRY, scratch + (size_t)i * ES_ENTRY, ES_ENTRY);
    return kept;
}

/* 0x94BE10's detour. A family no larger than its plan array, or an array that is not one of the plan's (0x90E150 passes
 * its own, for one row), goes straight on. */
static uint64_t EsSelect(void* a, void* roster, const uint8_t* rows, uint32_t count, ...)
{
    va_list stack; /* the seven arguments the game passes on the stack, as they lie there */
    va_start(stack, count);
    uint8_t* out = va_arg(stack, uint8_t*);
    uint32_t* outCount = va_arg(stack, uint32_t*);
    uint64_t level = va_arg(stack, uint64_t), players = va_arg(stack, uint64_t);
    void* tags = va_arg(stack, void*);
    uint64_t flagA = va_arg(stack, uint64_t), flagB = va_arg(stack, uint64_t);
    va_end(stack);
    EsSelectFn next = (EsSelectFn)g_esSelectOriginal;
    uint32_t cap = 0;
    for (size_t k = 0; k < ES_KINDS; k++) if ((uintptr_t)outCount - (uintptr_t)out == kEsKinds[k].span) cap = kEsKinds[k].cap;
    if (cap == 0 || count <= cap || count > g_esScratchRows) return next(a, roster, rows, count, out, outCount, level, players, tags, flagA, flagB);
    AcquireSRWLockExclusive(&g_esLock);
    uint32_t got = 0;
    uint64_t result = next(a, roster, rows, count, g_esScratch, &got, level, players, tags, flagA, flagB);
    if (got > cap) InterlockedExchangeAdd(&g_esLeftOut, (LONG)(got - cap));
    *outCount = got <= cap ? got : EsKeep(out, g_esScratch, got, cap);
    if (got <= cap) memcpy(out, g_esScratch, (size_t)got * ES_ENTRY);
    ReleaseSRWLockExclusive(&g_esLock);
    return result;
}

/* ---- growing the families -------------------------------------------------------------------------------------------- */

static int EsJoins(const EsSquad* s, uint32_t f, size_t k) { return s->state >= 0 && s->kind == kEsKinds[k].kind && (s->factions & kErFactions[f].bit); }

/* Whether faction f's grown roster has a row of the group: a squad of a group with no row would never be sent, and
 * one the mod meant to exist is a mistake worth a line. */
static int EsGroupIn(uint32_t f, uint32_t key)
{
    const ErGrown* g = &g_erGrown[f];
    for (uint64_t r = 0; g->rows != NULL && r < g->rowCount; r++) if (memcmp(g->rows + r * ER_ROW, &key, 4) == 0) return 1;
    return 0;
}

/* A squad's row. Level 10 covers level 11 where difficulty.c already widened the game's rows (moved); else it widens
 * this one with them. The fields the SDK does not set are those every game squad of the kind has. */
static void EsRow(uint8_t* row, const EsSquad* s, int moved)
{
    int outpost = s->kind == HF_SQUAD_OUTPOST;
    uint32_t highest = moved && s->highest == DIF_VANILLA ? DIF_VANILLA + 1 : s->highest, id = outpost ? s->keys[0] : ES_ID, kind = ES_PATROL_KIND;
    uint64_t entity = outpost ? 0 : ES_SQUAD_ENTITY;
    float closest = 125.0f, farthest = 175.0f;
    memset(row, 0, ES_ROW);
    for (int i = 0; i < HF_SQUAD_MEMBERS && s->keys[i] != 0; i++)
    {
        memcpy(row + 12 * i, &s->keys[i], 4);
        memcpy(row + 12 * i + 4, &s->counts[i], 4);
    }
    memcpy(row + 0x60, s->tags, sizeof s->tags);
    memcpy(row + 0xE0, &s->weight, 4);
    memcpy(row + 0xE4, &s->cost, 4);
    memcpy(row + 0xEC, &s->lowest, 4);
    memcpy(row + 0xF0, &highest, 4);
    memcpy(row + 0xF4, &s->lowestPlayers, 4);
    memcpy(row + 0xF8, &s->highestPlayers, 4);
    memcpy(row + 0xFC, &id, 4);
    memcpy(row + 0x100, &entity, 8);
    memcpy(row + 0x108, &closest, 4);
    memcpy(row + 0x10C, &farthest, 4);
    memcpy(row + 0x110, &kind, 4);
}

/* Whether squad s joins faction f's family k: each of its groups needs a row there (logged once when not). */
static int EsFits(EsSquad* s, uint32_t f, size_t k, int tell)
{
    if (!EsJoins(s, f, k) || ModGone(s->mod) || !s->mod->loaded) return 0;
    for (int i = 0; i < HF_SQUAD_MEMBERS && s->keys[i] != 0; i++)
    {
        if (EsGroupIn(f, s->keys[i])) continue;
        if (tell) ApiLog(s->mod, "add_enemy_squad(%s): roster %u has no enemy in its group %08x; not added there", s->name, kErFactions[f].roster, s->keys[i]);
        return 0;
    }
    return 1;
}

/* Faction f's family k with the mods' squads after the game's: the pointer before the count, as the unit rows'. */
static const char* EsGrowFamily(uintptr_t game, uint32_t f, size_t k, int moved)
{
    uint64_t pair[2], joining = 0;
    uintptr_t at = game + kRosters[kErFactions[f].roster].descriptor + 0x20 + 0x10 * kEsKinds[k].family;
    for (int i = 0; i < g_erSquadCount; i++) joining += EsFits(EsAt(i), f, k, 1) != 0;
    if (joining == 0) return NULL;
    if (SafeRead(at, pair, sizeof pair) || pair[0] == 0 || pair[1] != kEsKinds[k].rows[f]) return "an enemy squad family is not the reviewed one";
    size_t bytes = (size_t)(pair[1] + joining) * ES_ROW;
    uint8_t* block = (uint8_t*)VirtualAlloc(NULL, bytes + (size_t)joining * sizeof(int), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (block == NULL || SafeRead((uintptr_t)pair[0], block, (size_t)pair[1] * ES_ROW)) return "no memory for a larger enemy squad family";
    EsGrown* g = &g_esGrown[f][k];
    g->rows = block; g->game = g->count = pair[1]; g->owner = (int*)(void*)(block + bytes);
    for (int i = 0; i < g_erSquadCount; i++)
    {
        if (!EsFits(EsAt(i), f, k, 0)) continue;
        EsRow(block + g->count * ES_ROW, EsAt(i), moved);
        g->owner[g->count++ - g->game] = i;
        EsAt(i)->rows++;
    }
    MemoryBarrier();
    uint64_t rows = (uint64_t)(uintptr_t)block;
    if (ApiWrite(at, &rows, 8) || ApiWrite(at + 8, &g->count, 8)) return "an enemy squad family could not be replaced";
    return NULL;
}

/* The plan builder's hook. NULL, or why not. The native fixture replaces it. */
static const char* EsHookGame(uintptr_t game)
{
    if (!PinMatches(game + ES_SELECT, kEsSelectPin, sizeof kEsSelectPin)) return "the enemy squad selection is not the reviewed one";
    return LoaderHook("enemy squads: a plan array's bound", game + ES_SELECT, (void*)EsSelect, &g_esSelectOriginal) ? "the enemy squad selection could not be hooked" : NULL;
}
static const char* (*g_esHook)(uintptr_t game) = EsHookGame;

/* The hook, with a scratch array for the largest family. NULL, or why not. */
static const char* EsHook(uintptr_t game, uint32_t rows)
{
    if (g_esSelectOriginal != NULL) return NULL;
    g_esScratch = (uint8_t*)VirtualAlloc(NULL, (size_t)rows * ES_ENTRY, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return g_esScratch == NULL ? "no memory for the enemy squad selection" : g_esHook(game);
}

/* What each squad got, in its mod's log. */
static void EsReport(const char* why)
{
    for (int i = 0; i < g_erSquadCount; i++)
    {
        EsSquad* s = EsAt(i);
        if (s->state != 0) continue;
        s->state = s->rows > 0 ? 1 : -1;
        if (s->rows > 0) ApiLog(s->mod, "enemy squad %s: in %d roster(s)", s->name, s->rows);
        else ApiLog(s->mod, "add_enemy_squad(%s): not added%s%s", s->name, why ? ": " : "", why ? why : "");
    }
}

/* Every squad into its factions' families, the hook first (a family is not grown without its bound). NULL, or why not. */
static const char* EnemySquadsGrow(uintptr_t game, int moved)
{
    if (g_erSquadCount == 0) return NULL;
    uint32_t most = 0;
    for (uint32_t f = 0; f < ER_FACTIONS; f++)
        for (size_t k = 0; k < ES_KINDS; k++) most = kEsKinds[k].rows[f] > most ? kEsKinds[k].rows[f] : most;
    g_esScratchRows = most + (uint32_t)g_erSquadCount;
    const char* why = EsHook(game, g_esScratchRows);
    for (uint32_t f = 0; why == NULL && f < ER_FACTIONS; f++)
        for (size_t k = 0; why == NULL && k < ES_KINDS; k++) why = EsGrowFamily(game, f, k, moved);
    EsReport(why);
    return why;
}

/* The mod squads after the game's in a roster's family (difficulty.c counts the game's rows only). */
static uint64_t EnemySquadsIn(size_t roster, uint32_t family)
{
    for (uint32_t f = 0; f < ER_FACTIONS; f++)
        for (size_t k = 0; k < ES_KINDS; k++)
            if (kErFactions[f].roster == roster && kEsKinds[k].family == family) return g_esGrown[f][k].count - g_esGrown[f][k].game;
    return 0;
}

static uint64_t EnemySquadsAdded(void)
{
    uint64_t added = 0;
    for (int i = 0; i < g_erSquadCount; i++) added += EsAt(i)->state == 1;
    return added;
}

static void EnemySquadsTell(const char* why)
{
    for (int i = 0; why != NULL && i < g_erSquadCount; i++)
        if (EsAt(i)->state == 0) { EsAt(i)->state = -1; ApiLog(EsAt(i)->mod, "add_enemy_squad(%s): not added: %s", EsAt(i)->name, why); }
}

static void EnemySquadsMark(const HfMod* mod)
{
    for (int i = 0; i < g_erSquadCount; i++) if (EsAt(i)->mod == mod) EsAt(i)->gone = 1;
}

/* The squads of mods taken out (enemy_rosters.c, once no mission runs): a row with no member is never selected. */
static void EnemySquadsForget(void)
{
    for (uint32_t f = 0; f < ER_FACTIONS; f++)
        for (size_t k = 0; k < ES_KINDS; k++)
            for (uint64_t r = g_esGrown[f][k].game; r < g_esGrown[f][k].count; r++)
                if (EsAt(g_esGrown[f][k].owner[r - g_esGrown[f][k].game])->gone) memset(g_esGrown[f][k].rows + r * ES_ROW, 0, 4);
}

/* What the plan arrays left out, at most every 10 s. */
static void EnemySquadsWatch(void)
{
    LONG left = g_esLeftOut;
    if (left == g_esLeftOutReported || GetTickCount64() - g_esReportedAt < 10000) return;
    g_esReportedAt = GetTickCount64();
    g_esLeftOutReported = left;
    Note("enemy squads: %ld squad(s) that fit a mission left out of its plan so far; a mission takes 96 kinds of patrol, 64 of encounter, 32 of outpost and 8 of the rest", (long)left);
}
