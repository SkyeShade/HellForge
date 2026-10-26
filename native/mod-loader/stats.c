/* Declarative scalar edits. Included after the registries. No value hooks or periodic rewrites.
 * Component edits run on the entity loader's thread; settings edits run once after table installation.
 * A reviewed component index/header/stride/record or settings row identity guards every named field.
 * Raw and legacy writes additionally compare the original field value. */
#define MAX_STATS 8192
typedef struct StatEdit
{
    HfMod* mod;
    char kind[32], component[64], label[256];
    uint64_t resource;
    int64_t record;
    uint32_t offset, value, expect;
    int hasExpect, own, shared, unverified, floating, signedValue;
    uintptr_t address;
    uint32_t original;
    int applied, hasBaseline;
    int ref;        /* its field's reference kind (refs.c; -1 not looked up yet) */
    int refWaiting; /* a reference waiting for its packages (StatsRefsTick writes it) */
    uint32_t size;  /* 1 for a u8 (a component's flag), else 0 or 4: four bytes */
} StatEdit;
/* The bytes a stats edit or observation reads and writes: 1 (u8) or 4. */
static uint32_t StatSize(const StatEdit* e) { return e->size == 1 ? 1u : 4u; }
static int RefStat(StatEdit* e, uintptr_t address, uint32_t original);
static uintptr_t PerkRow(const char* kind, uint64_t id, int* ready); /* perks.c */
static int g_perksDone; /* the stats pass wrote the armor perk edits (or refused them) */
static void ShotsNoteLocal(uint64_t resource, int slot, uint32_t before, uint32_t after); /* shots.c */
static int ShotsSlotOf(const char* component, uint32_t offset);
static int StatsTableKind(const char* kind)
{
    return !strcmp(kind, "ProjectileSettings") || !strcmp(kind, "DamageInfo") || !strcmp(kind, "StratagemDefinition") ||
        !strcmp(kind, "BoosterSettings") || !strcmp(kind, "PassiveModifier") || !strcmp(kind, "PassiveStat") || !strcmp(kind, "MeleeHitVolume") ||
        !strcmp(kind, "DifficultySettings") || DirectorKind(kind);
}
static StatEdit g_stats[MAX_STATS];
static int g_statCount;
static volatile LONG g_statsComponentsReady;
static int g_statsTablesDone[3];
static SRWLOCK g_statsLock = SRWLOCK_INIT;
static unsigned g_statsGeneration;
static unsigned g_statsAppliedGeneration = UINT32_MAX;
static int g_statsLoading;
/* The fields the launcher's editors read (the plan's observe lines, one per stats-schema field): as many as the plan
 * has, grown as it is read. */
static StatEdit* g_statsProbes;
static int g_statsProbeCount, g_statsProbeCapacity, g_statsProbesSkipped;

static StatEdit* StatsProbeSlot(void)
{
    if (g_statsProbeCount == g_statsProbeCapacity)
    {
        int capacity = g_statsProbeCapacity ? g_statsProbeCapacity * 2 : 4096;
        StatEdit* grown = capacity <= (1 << 20) ? (StatEdit*)realloc(g_statsProbes, (size_t)capacity * sizeof *grown) : NULL;
        if (grown == NULL) return NULL;
        g_statsProbes = grown;
        g_statsProbeCapacity = capacity;
    }
    return &g_statsProbes[g_statsProbeCount++];
}
static char g_statsSchema[65];

static int StatsHasComponents(void)
{
    for (int i = 0; i < g_statCount; i++) if (!strcmp(g_stats[i].kind, "component")) return 1;
    for (int i = 0; i < g_statsProbeCount; i++) if (!strcmp(g_statsProbes[i].kind, "component")) return 1;
    return 0;
}

static int StatsComponentsPending(void) { return g_registryActive && StatsHasComponents() && !g_statsComponentsReady; }

static void StatsRefuse(StatEdit* e, const char* reason)
{
    ApiLog(e->mod, "stats %s: refused: %s", e->label, reason);
}

static int StatsFiniteBits(uint32_t bits) { float value; memcpy(&value, &bits, 4); return isfinite(value); }

/* The field's value before any edit: the baseline, else what it holds now; an earlier edit of the same field's original
 * (a conflict, logged: the later mod wins). */
static uint32_t StatsOriginal(const StatEdit* e, uintptr_t address, uint32_t current)
{
    uint32_t original = e->hasBaseline ? e->original : current;
    for (const StatEdit* earlier = g_stats; earlier < e; earlier++)
        if (earlier->applied && earlier->address == address)
        {
            original = earlier->original;
            ApiLog(e->mod, "stats %s: conflicts with %s; later mod wins", e->label, earlier->mod->id);
        }
    return original;
}

/* this machine's own swap of a weapon's projectile (co-op: friends' copies of its other players' shots, shots.c) */
static void StatsNoteShots(const StatEdit* e, uint32_t original)
{
    if (!strcmp(e->kind, "component") && !e->own && ShotsSlotOf(e->component, e->offset) >= 0)
        ShotsNoteLocal(e->resource, ShotsSlotOf(e->component, e->offset), original, e->value);
}

static void StatsWrite(StatEdit* e, uintptr_t address)
{
    uint32_t current = 0, original, size = StatSize(e);
    if (!address || SafeRead(address, &current, size)) { StatsRefuse(e, "field is unreadable"); return; }
    original = StatsOriginal(e, address, current);
    if (e->hasExpect && original != e->expect) { StatsRefuse(e, "original value differs from expect"); return; }
    if (e->floating && !StatsFiniteBits(current)) { StatsRefuse(e, "current float is not finite"); return; }
    if (RefStat(e, address, original) != 0 /* REF_OK */) return;
    if (e->shared && !e->own) ApiLog(e->mod, "stats %s: shared record; all consumers are affected", e->label);
    if (e->unverified) ApiLog(e->mod, "stats %s: gameplay effect is unverified", e->label);
    if (ApiWrite(address, &e->value, size) || SafeRead(address, &current, size) || current != e->value)
    { StatsRefuse(e, "write verification failed"); return; }
    e->address = address; e->original = original; e->applied = 1;
    StatsNoteShots(e, original);
    ApiLog(e->mod, "stats %s: applied (%08x -> %08x)%s", e->label, original, e->value, e->own ? " to its own record" : "");
}

/* The index row naming the resource: its record in *selected. 0 when none or several do, a row does not read, or the
 * row's flags are set. */
static int StatsSelectedRecord(const Component* c, const uint8_t* table, uint64_t resource, uint32_t* selected)
{
    uint32_t owners = 0;
    for (uint32_t i = 0; i < c->indices; i++)
    {
        uint8_t entry[16]; uint64_t id; uint32_t record, flags;
        if (SafeRead((uintptr_t)table + i * 16u, entry, 16)) return 0;
        memcpy(&id, entry, 8); memcpy(&record, entry + 8, 4); memcpy(&flags, entry + 12, 4);
        if (id == resource) { if (flags) return 0; *selected = record; owners++; }
    }
    return owners == 1;
}

/* A preceding own edit for this entity has already checked its vanilla record identity. Reuse that clone. */
static int StatsCloned(const StatEdit* e, uintptr_t record)
{
    for (const StatEdit* earlier = g_stats; earlier < e; earlier++)
        if (earlier->applied && earlier->own && earlier->resource == e->resource && !strcmp(earlier->component, e->component) &&
            earlier->address - earlier->offset == record) return 1;
    return 0;
}

/* Whether the field at `address` holds e's expect as StatsWrite will see it (the baseline, or an earlier edit's
 * original). Checked before cloning too; a refused own edit must not redirect the entity. */
static int StatsExpectHolds(const StatEdit* e, uintptr_t address, uint32_t size)
{
    uint32_t current = 0;
    if (SafeRead(address, &current, size)) return 0;
    uint32_t original = e->hasBaseline ? e->original : current;
    for (const StatEdit* earlier = g_stats; earlier < e; earlier++)
        if (earlier->applied && earlier->address == address) original = earlier->original;
    return original == e->expect;
}

/* An own edit's field in the entity's own record (OwnRecord), the earlier edits' addresses moved with the table when the
 * copy moved it. */
static uintptr_t StatsOwnRecord(StatEdit* e, uint8_t* state, const Component* c, uint8_t* table, uint32_t extent)
{
    uint8_t* copy = OwnRecord(state, c->index, c->stride, e->resource, e->resource);
    uint8_t* moved = StatePointer(state, REG_TABLES + 8 * c->index);
    if (copy && moved != table)
        for (StatEdit* earlier = g_stats; earlier < e; earlier++)
            if (earlier->applied && !strcmp(earlier->component, e->component) &&
                earlier->address >= (uintptr_t)table + c->recordOffset && earlier->address < (uintptr_t)table + extent)
                earlier->address = (uintptr_t)moved + (earlier->address - (uintptr_t)table);
    return copy ? (uintptr_t)copy + e->offset : 0;
}

static uintptr_t StatsComponent(StatEdit* e, uint8_t* state)
{
    const Component* c = LastComponentNamed(e->component);
    uint32_t size = StatSize(e);
    if (!c || c->stride < 4 || e->offset > c->stride - size || (size == 4 && e->offset % 4) || c->recordOffset != c->indices * 16u) return 0;
    uint8_t* table = StatePointer(state, REG_TABLES + 8 * c->index);
    uint8_t header[32];
    if (!table || SlotsOf(c->index) != c->indices || SafeRead((uintptr_t)table - 32, header, 32) ||
        memcmp(header, &c->index, 4) || memcmp(header + 4, c->header, 28)) return 0;
    uint32_t extent; memcpy(&extent, header + 20, 4);
    if ((uint64_t)c->recordOffset + (uint64_t)c->records * c->stride > extent) return 0;
    uint32_t selected = 0;
    if (!StatsSelectedRecord(c, table, e->resource, &selected)) return 0;
    uintptr_t record = (uintptr_t)table + c->recordOffset + (uintptr_t)selected * c->stride;
    if (StatsCloned(e, record)) return record + e->offset;
    if (selected >= c->records || (e->record >= 0 && selected != (uint64_t)e->record)) return 0;
    if (e->hasExpect && !StatsExpectHolds(e, record + e->offset, size)) return 0;
    if (e->own) return StatsOwnRecord(e, state, c, table, extent);
    return record + e->offset;
}

#include "stats-tables.c"

/* A component edit's guard: the vanilla field's value, read before any edit or clone. */
static void StatsBaseline(StatEdit* e, uint8_t* state)
{
    if (strcmp(e->kind, "component") || !e->mod->loaded) return;
    if (strcmp(g_build, "25480438")) { StatsRefuse(e, "unsupported build"); return; }
    int own = e->own; e->own = 0;
    uintptr_t address = StatsComponent(e, state); e->own = own;
    e->original = 0;
    if (!address || SafeRead(address, &e->original, StatSize(e))) StatsRefuse(e, "component layout, ownership or expected value differs from the reviewed record");
    else e->hasBaseline = 1;
}

static void StatsApplyComponent(StatEdit* e, uint8_t* state)
{
    if (strcmp(e->kind, "component") || !e->mod->loaded || !e->hasBaseline) return;
    uintptr_t address = StatsComponent(e, state);
    if (!address) StatsRefuse(e, "component layout, ownership or expected value differs from the reviewed record");
    else StatsWrite(e, address);
}

static void StatsApplyComponents(uint8_t* state)
{
    AcquireSRWLockExclusive(&g_statsLock);
    if (g_statsLoading || g_statsAppliedGeneration == g_statsGeneration)
    { ReleaseSRWLockExclusive(&g_statsLock); return; }
    g_statsAppliedGeneration = g_statsGeneration;
    for (int i = 0; i < g_statCount; i++)
        if (!strcmp(g_stats[i].kind, "component")) { g_stats[i].applied = 0; g_stats[i].address = 0; g_stats[i].hasBaseline = 0; }
    /* Capture guards before any edit or clone. Moving a shared table must not change what expect means. */
    for (int i = 0; i < g_statCount; i++) StatsBaseline(&g_stats[i], state);
    for (int i = 0; i < g_statCount; i++) StatsApplyComponent(&g_stats[i], state);
    InterlockedExchange(&g_statsComponentsReady, 1);
    ReleaseSRWLockExclusive(&g_statsLock);
}

static void StatsBeginComponents(void)
{
    AcquireSRWLockExclusive(&g_statsLock);
    g_statsGeneration++;
    g_statsLoading = 1;
    InterlockedExchange(&g_statsComponentsReady, 0);
    ReleaseSRWLockExclusive(&g_statsLock);
}

static void StatsFinishComponents(uint8_t* state)
{
    AcquireSRWLockExclusive(&g_statsLock);
    g_statsLoading = 0;
    ReleaseSRWLockExclusive(&g_statsLock);
    StatsApplyComponents(state);
}

/* phase: projectile, damage, stratagem. */
static const char* StatsTableName(int phase) { return phase == 0 ? "ProjectileSettings" : phase == 1 ? "DamageInfo" : "StratagemDefinition"; }
/* The phase's table: a moved one takes precedence over the original. */
static uintptr_t StatsTableOf(int phase, uintptr_t game)
{
    return phase == 0 ? ProjectileTable(game) : phase == 1 ? (g_damageTable ? (uintptr_t)g_damageTable : game + DMG_TABLE) :
        (g_stratagemTable ? (uintptr_t)g_stratagemTable : game + STR_TABLE);
}

/* The vanilla row among slots [first, last) whose id is `record` (the stratagems' stable id at +4, else its type): the
 * row, 0 when not exactly one is. */
static uintptr_t StatsTableRow(int phase, uintptr_t table, uint32_t first, uint32_t last, int64_t record)
{
    uintptr_t found = 0;
    int matches = 0;
    for (uint32_t slot = first; slot < last; slot++)
    {
        uintptr_t row = 0; uint32_t id = 0, type = 0;
        if (SafeRead(table + slot * 8u, &row, 8) || !row || SafeRead(row, &type, 4) || type != slot) continue;
        if (phase == 2) { if (SafeRead(row + 4, &id, 4)) continue; } else id = type;
        if (record == id) { found = row; matches++; }
    }
    return matches == 1 ? found : 0;
}

static void StatsApplyRow(StatEdit* e, int phase, uintptr_t table, uint32_t count, uint32_t stride)
{
    if (strcmp(g_build, "25480438") || e->own || e->offset < 4 || e->offset > stride - 4 || e->offset % 4)
    { StatsRefuse(e, "unsupported build or table layout"); return; }
    uintptr_t found = StatsTableRow(phase, table, 1, count, e->record);
    uint8_t layout[STR_ROW];
    if (!found || SafeRead(found, layout, stride)) { StatsRefuse(e, "table row identity/layout is not unique or readable"); return; }
    StatsWrite(e, found + e->offset);
}

/* phase: projectile, damage, stratagem. Moved tables always take precedence over the original. */
static void StatsApplyTable(int phase, uintptr_t game)
{
    if (g_statsTablesDone[phase]) return;
    g_statsTablesDone[phase] = 1;
    const char* kind = StatsTableName(phase);
    uintptr_t table = StatsTableOf(phase, game);
    uint32_t count = phase == 0 ? PRJ_VANILLA : phase == 1 ? DMG_VANILLA : STR_VANILLA;
    uint32_t stride = phase == 0 ? PRJ_ROW : phase == 1 ? DMG_ROW : STR_ROW;
    for (int i = 0; i < g_statCount; i++)
    {
        StatEdit* e = &g_stats[i];
        if (strcmp(e->kind, kind) || !e->mod->loaded) continue;
        StatsApplyRow(e, phase, table, count, stride);
    }
}

static HfMod* StatsModNamed(const char* id)
{
    HfMod* mod = NULL;
    for (int i = 0; i < g_modCount; i++) if (!strcmp(g_mods[i].id, id)) mod = &g_mods[i];
    return mod;
}

static int StatsStorageKnown(const char* storage)
{
    return !strcmp(storage, "f32") || !strcmp(storage, "u32") || !strcmp(storage, "i32") || !strcmp(storage, "u8");
}

/* A stat line's resource, record, offset, value and expect into e (its size set). 1 when one does not parse. */
static int StatsParseNumbers(StatEdit* e, char** f)
{
    char* end;
    e->resource = strtoull(f[4], &end, 16); if (*end) return 1;
    e->record = strtoll(f[5], &end, 10); if (*end || e->record < -1 || e->record > UINT32_MAX) return 1;
    uint64_t offset = strtoull(f[6], &end, 10); if (*end || offset > UINT32_MAX || (offset % 4 && strcmp(f[2], "EntityDelta") && e->size != 1)) return 1; e->offset = (uint32_t)offset;
    uint64_t bits = strtoull(f[8], &end, 16); if (*end || bits > (e->size == 1 ? 0xFFu : UINT32_MAX)) return 1; e->value = (uint32_t)bits;
    e->hasExpect = strcmp(f[9], "-") != 0;
    if (e->hasExpect) { bits = strtoull(f[9], &end, 16); if (*end || bits > UINT32_MAX) return 1; e->expect = (uint32_t)bits; }
    return 0;
}

/* own, shared and unverified: each "0" or "1". */
static int StatsFlagsValid(char** f)
{
    for (int i = 10; i <= 12; i++) if (strcmp(f[i], "0") && strcmp(f[i], "1")) return 0;
    return 1;
}

static int StatsParse(char** f, int n)
{
    if (n != 14 || g_statCount >= MAX_STATS) return 1;
    StatEdit e = {0};
    e.mod = StatsModNamed(f[1]);
    if (!e.mod || strlen(f[2]) >= sizeof e.kind || strlen(f[3]) >= sizeof e.component || strlen(f[13]) >= sizeof e.label) return 1;
    if (strcmp(f[2], "component") && !StatsTableKind(f[2]) && StatsExtraKind(f[2]) < 0) return 1;
    if (!StatsStorageKnown(f[7])) return 1;
    if (!strcmp(f[7], "u8") && strcmp(f[2], "component")) return 1; /* one-byte flags are components' only */
    e.size = !strcmp(f[7], "u8") ? 1 : 4;
    snprintf(e.kind, sizeof e.kind, "%s", f[2]); snprintf(e.component, sizeof e.component, "%s", f[3]);
    e.ref = -1;
    snprintf(e.label, sizeof e.label, "%s", f[13]);
    if (StatsParseNumbers(&e, f)) return 1;
    if (!StatsFlagsValid(f)) return 1;
    e.own = f[10][0] == '1'; e.shared = f[11][0] == '1'; e.unverified = f[12][0] == '1'; e.floating = !strcmp(f[7], "f32");
    if (e.floating && !StatsFiniteBits(e.value)) return 1;
    g_stats[g_statCount++] = e;
    return 0;
}

/* One phase's table, once installed (or refused). */
static void StatsTableReady(int phase, uintptr_t game)
{
    if (g_statsTablesDone[phase]) return;
    int state = phase == 0 ? g_projectileState : phase == 1 ? g_damageState : g_stratagemState;
    const char* kind = StatsTableName(phase);
    if (state < 0)
    {
        for (int i = 0; i < g_statCount; i++) if (!strcmp(g_stats[i].kind, kind)) StatsRefuse(&g_stats[i], "table installation was refused");
        g_statsTablesDone[phase] = 1;
    }
    else if (state == 1)
    {
        uintptr_t table = StatsTableOf(phase, game);
        uintptr_t first = 0;
        if (!SafeRead(table + 8, &first, 8) && first) StatsApplyTable(phase, game);
    }
}

static void StatsTablesReady(uintptr_t game)
{
    if (!game) return;
    for (int phase = 0; phase < 3; phase++) StatsTableReady(phase, game);
}

#include "stats-snapshot.c"
