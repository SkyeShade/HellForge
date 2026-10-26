/* Retained allocation adapters, ported from HD2Runtime core/settings and attachment_writes.prove.
 * Only framing, counts, relocation and member offsets are generated. No extracted game values are shipped. */
typedef struct StatsGroup { uint32_t offset, type, extent, root, rowOffset, count; } StatsGroup;
typedef struct StatsLayout { const char* kind; uint32_t size, stride, count; StatsGroup groups[4]; } StatsLayout;
typedef struct StatsDeltaLayout { uint32_t size, type, header; uint32_t offsets[5], counts[5]; } StatsDeltaLayout;
#include "stats-layouts.h"
#define STATS_EXTRA_COUNT (sizeof kStatsLayouts / sizeof kStatsLayouts[0] + 1)
static uintptr_t g_statsAllocations[STATS_EXTRA_COUNT];
static int g_statsExtraDone[STATS_EXTRA_COUNT];

/* The entity delta table's header (head: its first 7 words) and its arrays as reviewed. */
static int StatsDeltaValid(uintptr_t base, const uint32_t* head)
{
    const StatsDeltaLayout* d = &kStatsDelta;
    if (head[0] != d->type || head[1] != 0x444C444Cu || head[2] != 1 || head[3] != d->type ||
        head[4] != d->size - 28 || head[5] != 1 || head[6]) return 0;
    uint64_t arrays[10];
    if (SafeRead(base + d->header, arrays, sizeof arrays)) return 0;
    /* In place, or moved by the loader with rows appended (difficulty.c's countdown delta). */
    for (int i = 0; i < 5; i++)
        if (!(arrays[i * 2] == base + d->header + d->offsets[i] && arrays[i * 2 + 1] == d->counts[i]) &&
            !(base + d->header == g_deltaTable && g_deltaArrays[i] && arrays[i * 2] == g_deltaArrays[i] &&
              arrays[i * 2 + 1] == (uint64_t)d->counts[i] + g_deltaAdded[i])) return 0;
    return 1;
}

/* A settings allocation's group as the layout has it, from *end (moved to the group's end): its header, and its rows
 * inside it. */
static int StatsGroupValid(uintptr_t base, const StatsLayout* d, const StatsGroup* g, uint32_t* end)
{
    uint32_t h[6]; uint64_t rows[2];
    if (g->offset != *end || SafeRead(base + g->offset, h, sizeof h) || h[0] != 0x444C444Cu || h[1] != 1 ||
        h[2] != g->type || h[3] != g->extent || h[4] != 1 || h[5]) return 0;
    *end = g->offset + 24 + g->extent;
    if (*end > d->size) return 0;
    if (g->root && (SafeRead(base + g->root, rows, sizeof rows) || rows[1] != g->count ||
        (rows[0] != g->rowOffset && rows[0] != base + g->root + g->rowOffset) ||
        (uint64_t)g->root + g->rowOffset + (uint64_t)g->count * d->stride > *end)) return 0;
    return 1;
}

static int StatsAllocationValid(uintptr_t base, int kind)
{
    uint32_t head[7];
    if (SafeRead(base, head, sizeof head)) return 0;
    if (kind == (int)STATS_EXTRA_COUNT - 1) return StatsDeltaValid(base, head);
    const StatsLayout* d = &kStatsLayouts[kind];
    if (head[0] != d->count) return 0;
    uint32_t end = 4;
    for (uint32_t i = 0; i < d->count; i++)
        if (!StatsGroupValid(base, d, &d->groups[i], &end)) return 0;
    return end == d->size;
}

/* A status effect's stat multiplier (a booster's: Experimental Infusion's incoming damage scale): the record is the
 * status type, the component the stat id, the offset index * 8 + 4 into the array its StatusEffectSettings row points to
 * (+104, its count +112; HD2Runtime booster_writes.lua status_multiplier). The array is in the same allocation. */
#define STATUS_MULTIPLIER "StatusMultiplier"

static int StatsExtraKind(const char* name)
{
    if (!strcmp(name, STATUS_MULTIPLIER)) name = "StatusEffectSettings";
    for (int i = 0; i < (int)STATS_EXTRA_COUNT - 1; i++) if (!strcmp(kStatsLayouts[i].kind, name)) return i;
    return !strcmp(name, "EntityDelta") ? (int)STATS_EXTRA_COUNT - 1 : -1;
}

/* A private allocation as each settings allocation still wanted: its size in range and its layout as reviewed. */
static void StatsAllocationCandidates(const MEMORY_BASIC_INFORMATION* info, int* matches, uintptr_t* candidates)
{
    for (int i = 0; i < (int)STATS_EXTRA_COUNT; i++)
    {
        uint32_t size = i == (int)STATS_EXTRA_COUNT - 1 ? kStatsDelta.size : kStatsLayouts[i].size;
        if (!g_statsExtraDone[i] && info->RegionSize >= size && info->RegionSize <= (size_t)size + 65536 &&
            StatsAllocationValid((uintptr_t)info->BaseAddress, i))
        { matches[i]++; candidates[i] = (uintptr_t)info->BaseAddress; }
    }
}

static void StatsFindAllocations(void)
{
    int matches[STATS_EXTRA_COUNT] = {0}; uintptr_t candidates[STATS_EXTRA_COUNT] = {0};
    uintptr_t at = 0x10000; MEMORY_BASIC_INFORMATION info;
    while (VirtualQuery((void*)at, &info, sizeof info))
    {
        uintptr_t next = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (PrivateAllocation(&info)) StatsAllocationCandidates(&info, matches, candidates);
        if (next <= at) break;
        at = next;
    }
    for (int i = 0; i < (int)STATS_EXTRA_COUNT; i++) if (!g_statsExtraDone[i])
        g_statsAllocations[i] = matches[i] == 1 ? candidates[i] : 0;
}

static uintptr_t StatsExtraRow(StatEdit* e);

/* A status multiplier's place: the array its StatusEffectSettings row points to (+104, its count +112), the stat at
 * the offset's index checked to be the component's. */
static uintptr_t StatsStatusRow(const StatEdit* e, uintptr_t base, int kind)
{
    StatEdit row = *e;
    snprintf(row.kind, sizeof row.kind, "StatusEffectSettings");
    row.offset = 104;
    uintptr_t at = StatsExtraRow(&row);
    uint64_t array = 0, count = 0; uint32_t stat = 0, index = (e->offset - 4) / 8; char* end;
    unsigned long expect = strtoul(e->component, &end, 10);
    if (!at || *end || e->offset < 4 || (e->offset - 4) % 8 || SafeRead(at, &array, 8) || SafeRead(at + 8, &count, 8) ||
        index >= count || array < base || array + (count * 8) > base + kStatsLayouts[kind].size ||
        SafeRead((uintptr_t)array + index * 8, &stat, 4) || stat != expect) return 0;
    return (uintptr_t)array + e->offset;
}

/* A settings row's field: the one row of the layout's groups whose id is the record. */
static uintptr_t StatsLayoutRow(const StatEdit* e, uintptr_t base, int kind)
{
    const StatsLayout* d = &kStatsLayouts[kind];
    if (e->offset < 4 || e->offset > d->stride - 4 || e->offset % 4) return 0;
    uintptr_t found = 0; int matches = 0;
    for (uint32_t i = 0; i < d->count; i++)
    {
        const StatsGroup* g = &d->groups[i];
        for (uint32_t row = 0; row < g->count; row++)
        {
            uintptr_t at = base + g->root + g->rowOffset + row * d->stride; uint32_t id;
            if (SafeRead(at, &id, 4)) return 0;
            if (id == e->record) { found = at; matches++; }
        }
    }
    return matches == 1 ? found + e->offset : 0;
}

/* Whether exactly one of the delta table's index rows names the entity, and names the record. */
static int StatsDeltaOwned(const uint64_t* arrays, const StatsDeltaLayout* d, const StatEdit* e)
{
    int owners = 0;
    for (uint32_t i = 0; i < d->counts[0]; i++)
    {
        uint8_t row[16]; uint64_t resource; uint32_t index;
        if (SafeRead(arrays[0] + i * 16, row, sizeof row)) return 0;
        memcpy(&resource, row, 8); memcpy(&index, row + 8, 4);
        if (resource == e->resource) { if (index != e->record) return 0; owners++; }
    }
    return owners == 1;
}

/* The record's settings (settings: their first and count): the one u32 delta of the component's member at the offset. */
static uintptr_t StatsDeltaFind(const uint64_t* arrays, const StatsDeltaLayout* d, const uint32_t* settings, uint32_t component,
    uint32_t member, uint32_t offset)
{
    int matches = 0; uintptr_t found = 0;
    for (uint32_t i = 0; i < settings[0]; i++)
    {
        uint32_t c[3];
        if (SafeRead(arrays[4] + (settings[1] + i) * 12, c, sizeof c) ||
            !c[2] || c[2] > 64 || (uint64_t)c[1] + c[2] > d->counts[3]) return 0;
        for (uint32_t j = 0; j < c[2]; j++)
        {
            uint32_t delta[3];
            if (SafeRead(arrays[6] + (c[1] + j) * 12, delta, sizeof delta) ||
                (uint64_t)delta[2] + delta[1] > d->counts[4]) return 0;
            if (c[0] == component && delta[0] == member && delta[1] == 4 && d->header + d->offsets[4] + delta[2] == offset)
            { matches++; found = arrays[8] + delta[2]; }
        }
    }
    return matches == 1 ? found : 0;
}

/* An entity delta's field ("<component>:<member>"), read through the table's own pointers: the loader may have moved
 * arrays 1 to 4. */
static uintptr_t StatsDeltaRow(const StatEdit* e, uintptr_t base)
{
    const StatsDeltaLayout* d = &kStatsDelta;
    uint64_t arrays[10];
    uint32_t component, member; char trailing;
    if (SafeRead(base + d->header, arrays, sizeof arrays)) return 0;
    if (sscanf(e->component, "%u:%u%c", &component, &member, &trailing) != 2 || e->record < 0 || (uint64_t)e->record >= d->counts[1]) return 0;
    if (!StatsDeltaOwned(arrays, d, e)) return 0;
    uint32_t settings[2];
    if (SafeRead(arrays[2] + e->record * 8, settings, sizeof settings) ||
        !settings[0] || settings[0] > 64 || (uint64_t)settings[1] + settings[0] > d->counts[2]) return 0;
    return StatsDeltaFind(arrays, d, settings, component, member, e->offset);
}

static uintptr_t StatsExtraRow(StatEdit* e)
{
    int kind = StatsExtraKind(e->kind);
    if (kind < 0 || e->own) return 0;
    uintptr_t base = g_statsAllocations[kind];
    if (!base || !StatsAllocationValid(base, kind)) return 0;
    if (!strcmp(e->kind, STATUS_MULTIPLIER)) return StatsStatusRow(e, base, kind);
    if (kind != (int)STATS_EXTRA_COUNT - 1) return StatsLayoutRow(e, base, kind);
    return StatsDeltaRow(e, base);
}

/* The booster table (HD2Runtime's research, booster-native-F5FEE03DCFDB.json; game.dll SizeOfImage 0x4744000): static in
 * game.dll's writable data at +0x32FDA90, 21 rows of 0x38 bytes by booster value (0 None, 1 Vitality ... 20 FreeEAT), each
 * row's id hash at +0. Usable once every row's id reads as reviewed: game.dll's data is readable only after it unpacks. */
#define BOOSTER_TABLE 0x32FDA90u
#define BOOSTER_ROW 0x38u
#define BOOSTER_ROWS 21u
#define BOOSTER_IMAGE 0x4744000u
static const uint32_t kBoosterIds[BOOSTER_ROWS] = {0xA3BD6B87u, 0x4233BB28u, 0x28D025B8u, 0xA57D4ECBu, 0xF6948661u, 0xF267C3ACu,
    0xFEB5A878u, 0x71BCCD81u, 0x3445BCA8u, 0xD0E0FCF2u, 0xA17C1852u, 0xE92E637Eu, 0xB8F17AF8u, 0x140A3C29u, 0x85763547u, 0xE21B9534u,
    0x2F7DE576u, 0x449FE15Au, 0x5F895979u, 0xD35B7855u, 0x485AAA8Au};
static int g_boostersDone;

static uintptr_t BoosterTable(void)
{
    size_t size = 0;
    uintptr_t game = ApiModuleBase("game.dll", &size);
    if (game == 0 || size != BOOSTER_IMAGE) return 0;
    for (uint32_t i = 0; i < BOOSTER_ROWS; i++)
    {
        uint32_t id;
        if (SafeRead(game + BOOSTER_TABLE + i * BOOSTER_ROW, &id, 4) || id != kBoosterIds[i]) return 0;
    }
    return game + BOOSTER_TABLE;
}

/* A booster's row by its value (1 to 20), or 0. */
static uintptr_t BoosterRow(uint64_t value)
{
    uintptr_t table = value >= 1 && value < BOOSTER_ROWS ? BoosterTable() : 0;
    return table ? table + (uintptr_t)value * BOOSTER_ROW : 0;
}

/* Stats edits of the booster table, once it is readable (refused if it never is within 60 s of the entity data). */
static void StatsBoostersReady(void)
{
    static ULONGLONG first;
    if (g_boostersDone) return;
    ULONGLONG now = GetTickCount64();
    if (!first) first = now;
    uintptr_t table = BoosterTable();
    if (!table && now - first < 60000) return;
    g_boostersDone = 1;
    for (int i = 0; i < g_statCount; i++)
    {
        StatEdit* e = &g_stats[i];
        if (strcmp(e->kind, "BoosterSettings") || !e->mod->loaded) continue;
        if (!table || strcmp(g_build, "25480438")) StatsRefuse(e, "the booster table is not as reviewed");
        else if (e->own || e->record < 1 || e->record >= (int64_t)BOOSTER_ROWS || e->offset < 4 || e->offset > BOOSTER_ROW - 4 || e->offset % 4)
            StatsRefuse(e, "no such booster row or field");
        else StatsWrite(e, table + (uintptr_t)e->record * BOOSTER_ROW + e->offset);
    }
}

/* The melee hit volumes (HD2R Editor's Melee rows; melee_data.h, scripts/ExportMelee.py): static records in game.dll's
 * data by RVA, each checked by its type hash at +0, readable once game.dll unpacks. */
#include "melee_data.h"
static int g_meleeDone;
static uintptr_t GameImage(size_t* size) { return ApiModuleBase("game.dll", size); }
/* game.dll's image; the native fixture replaces it. */
static uintptr_t (*g_gameImage)(size_t* size) = GameImage;

/* A hit volume's record by its RVA (the record id), or 0: not one, or game.dll's data not as reviewed (yet). */
static uintptr_t MeleeVolume(uint64_t rva)
{
    size_t size = 0;
    uintptr_t game = g_gameImage(&size);
    if (game == 0 || size != BOOSTER_IMAGE) return 0;
    for (size_t i = 0; i < sizeof kMeleeVolumes / sizeof kMeleeVolumes[0]; i++)
    {
        uint32_t hash;
        if (kMeleeVolumes[i].rva != rva) continue;
        return !SafeRead(game + kMeleeVolumes[i].rva, &hash, 4) && hash == kMeleeVolumes[i].hash ? game + kMeleeVolumes[i].rva : 0;
    }
    return 0;
}

static uintptr_t TableRowOf(const char* component, uint64_t id, int* ready); /* fields.c */

/* One difficulty, roster or composition edit, checked as a live edit is. */
static void StatsDirectorEdit(StatEdit* e)
{
    uint32_t stride = DirectorStride(e->kind), size = StatSize(e);
    if (!stride || !e->mod->loaded) return;
    int found = 1;
    uintptr_t row = strcmp(g_build, "25480438") || e->record < 0 ? 0 : TableRowOf(e->kind, (uint64_t)e->record, &found);
    const char* why = NULL;
    if (!row) { StatsRefuse(e, "no such row, or its table is not as reviewed"); return; }
    if (e->own || e->offset > stride - size || (size == 4 && e->offset % 4)) { StatsRefuse(e, "no such field in its row"); return; }
    if ((why = DirectorRefuses(e->kind, row, e->offset, &e->value, size)) != NULL) { StatsRefuse(e, why); return; }
    uint32_t before = 0;
    SafeRead(row + e->offset, &before, size);
    StatsWrite(e, row + e->offset);
    if (e->applied && !strcmp(e->kind, "DifficultySettings")) DifficultyMirror(row, e->offset, &before, &e->value, size);
}

/* Stats edits of the difficulty rows and the spawn director's rosters and compositions (director.c), once level 11 is in
 * place (or none was added) and game.dll's data reads as reviewed; refused if that never happens within 120 s. Each is
 * checked as a live edit is (a squad within the queue's capacity, ranges in order); a difficulty row of the game's own
 * table goes to every copy that held the same bytes. */
static int g_directorDone;
static void StatsDirectorReady(void)
{
    static ULONGLONG first;
    if (g_directorDone) return;
    ULONGLONG now = GetTickCount64();
    if (!first) first = now;
    int ready = 1;
    for (int i = 0; i < g_statCount && ready; i++)
        if (DirectorStride(g_stats[i].kind) && g_stats[i].mod->loaded) TableRowOf(g_stats[i].kind, (uint64_t)g_stats[i].record, &ready);
    if (!ready && now - first < 120000) return;
    g_directorDone = 1;
    for (int i = 0; i < g_statCount; i++) StatsDirectorEdit(&g_stats[i]);
}

/* Stats edits of the hit volumes, once game.dll's data reads as reviewed (refused if it never does within 60 s). */
static void StatsMeleeReady(void)
{
    static ULONGLONG first;
    if (g_meleeDone) return;
    ULONGLONG now = GetTickCount64();
    if (!first) first = now;
    int readable = MeleeVolume(kMeleeVolumes[0].rva) != 0;
    if (!readable && now - first < 60000) return;
    g_meleeDone = 1;
    for (int i = 0; i < g_statCount; i++)
    {
        StatEdit* e = &g_stats[i];
        if (strcmp(e->kind, "MeleeHitVolume") || !e->mod->loaded) continue;
        uintptr_t row = strcmp(g_build, "25480438") ? 0 : MeleeVolume((uint64_t)e->record);
        if (!row) StatsRefuse(e, "the hit volume is not as reviewed");
        else if (e->own || e->offset < 4 || e->offset > MELEE_VOLUME_SIZE - 4 || e->offset % 4) StatsRefuse(e, "no such hit volume field");
        else StatsWrite(e, row + e->offset);
    }
}

/* One kind's edits, once its allocation is found (or 20 s went by without it). */
static void StatsExtraKindReady(int kind, ULONGLONG waited)
{
    if (g_statsExtraDone[kind] || (!g_statsAllocations[kind] && waited < 20000)) return;
    g_statsExtraDone[kind] = 1;
    for (int i = 0; i < g_statCount; i++)
    {
        StatEdit* e = &g_stats[i];
        if (StatsExtraKind(e->kind) != kind || !e->mod->loaded) continue;
        uintptr_t address = !strcmp(g_build, "25480438") ? StatsExtraRow(e) : 0;
        if (!address) StatsRefuse(e, "settings allocation or delta ownership/layout is not uniquely as reviewed");
        else StatsWrite(e, address);
    }
}

/* Readiness polling only; a completed allocation is never written again. */
static void StatsExtraReady(void)
{
    static ULONGLONG first, last;
    ULONGLONG now = GetTickCount64();
    if (!first) first = now;
    if (last && now - last < 500) return;
    int pending = 0;
    for (int i = 0; i < (int)STATS_EXTRA_COUNT; i++) if (!g_statsExtraDone[i]) pending++;
    if (!pending) return;
    last = now; StatsFindAllocations();
    for (int kind = 0; kind < (int)STATS_EXTRA_COUNT; kind++) StatsExtraKindReady(kind, now - first);
}
