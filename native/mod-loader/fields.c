/* Named fields and deferred edits (SDK 6, docs/modding/content.md). Included by hellforge_modloader.c.
 *
 * A mod names a field with HF_FIELD (hellforge_components.h, generated from mappings/components.json): its component,
 * layout, offset, size and type. The loader takes only fields of its own table (sdk_fields.h, generated from the same
 * mappings), so a field the mappings do not name, or one from another layout, is refused with the mod's source line and
 * nothing is written. Accepted edits are kept and applied to the entity's own records each time the game loads its
 * entity data (ApplyFieldEdits, before HF_EVENT_ENTITIES_READY reaches the mods), so they are renewed if it reloads them.
 * A vanilla entity's record is edited only if no other entity shares it (a shared one is refused when the data loads:
 * stats mods, hellforge-stats.json, change shared records with their guards). Two mods editing the same field, or a
 * mod and a stats mod, are reported when the second edit is queued; edits apply in load order (stats first), so the
 * later one's value stays.
 *
 * Live edits (set_live, SDK 6; the launcher's live editor through live.c) are the same edits, applied at once: the
 * target is an entity by its resource id or a settings table row by its id (a projectile type, a damage id, a
 * stratagem type, a level...), resolved through the game's tables every time, never a stored address. A mod's later
 * live edit of the same field replaces its earlier one; a reset puts back the value from before it.
 *
 * Live edits of one field stack (the Lua bridge, engine 0.1.15): the editors' (the F8 editor, and Local Stats, which the launcher's editor and
 * the F8 editor save into) over Lua mods' (lua.<name>, lua_bridge.c) over code mods'. The record holds the top one; an
 * edit under another keeps its value and takes the field when the one above it goes, and each edit's original is the
 * value of the one under it. A Lua or code mod's live edit also stays under a Local Stats value the boot pass wrote
 * while the record holds it. get_live names a Lua or code mod's live edit as the field's holder (SDK 10), its value the
 * base. */

/* A field naming another table's row (mappings "ref"): its value must be such a row (refs.c). */
enum { REF_NONE, REF_PROJECTILE, REF_EXPLOSION, REF_STATUS, REF_DAMAGE, REF_MOUNT, REF_PICKUP };
enum { REF_OK, REF_WAIT, REF_REFUSED };
typedef struct SdkField { const char* name; uint16_t offset, size; uint32_t type; uint32_t applies; uint32_t ref; } SdkField;
typedef struct SdkComponent { const char* name; const char* component; uint32_t layout, stride; const SdkField* fields; int count; int table; int index; } SdkComponent;
#include "sdk_fields.h"
typedef struct SdkContent { uint32_t kind; const char* name; uint64_t id; } SdkContent;
#include "sdk_content.h"

#define FIELD_MAX_EDITS (1 << 20) /* a sanity bound: the list grows as mods and editors add edits */
#define FIELD_MAX_DEFS 64

enum { EDIT_COPY, EDIT_SET, EDIT_TAG };

typedef struct FieldEdit
{
    HfMod* mod;
    int kind;
    uint64_t entity, from;  /* from: EDIT_COPY's source entity */
    const SdkComponent* component;
    const SdkField* field;
    uint8_t value[8];       /* EDIT_SET: the bytes; EDIT_TAG: the tag */
    int present;            /* EDIT_TAG: added (1) or removed (0) */
    uint32_t line;
    int vanilla;            /* a vanilla entity's record (only if no other entity shares it) */
    int table;              /* a settings table row; entity is its id */
    int shared;             /* the record may be shared with other entities (live edits with HF_LIVE_SHARED, as stats files) */
    int live;               /* set_live: replaced by the mod's next live edit of the field, undone by a reset */
    int waiting;            /* its table is not loaded yet */
    int hasOriginal;        /* live: the bytes from before it, for a reset */
    uint8_t original[8];
    int failed;             /* refused on the last load (logged once per load) */
    int refNoted;           /* a reference's wait for its packages is logged */
    uintptr_t address;      /* where its bytes were when it last applied (get_stat names a live edit's holder by it) */
} FieldEdit;
static int RefEdit(FieldEdit* e, const uint8_t* at);

typedef struct EntityDefinition { const HfEntityDef* def; HfMod* mod; uint64_t entity; int busy; } EntityDefinition;
typedef struct BehaviourDefinition { const HfBehaviour* def; uint32_t id; } BehaviourDefinition;

static FieldEdit* g_edits; /* grown by EditRoom (an editor applying a preset makes thousands) */
static int g_editCapacity;
static int g_editCount;
static EntityDefinition g_entityDefs[FIELD_MAX_DEFS];
static int g_entityDefCount;
static BehaviourDefinition g_behaviourDefs[FIELD_MAX_DEFS];
static int g_behaviourDefCount;
static int g_fieldConflicts; /* edits that overlap another mod's or a stats mod's (reported) */
static int g_editsWaiting;   /* some edit waits for its table */
static SRWLOCK g_fieldsLock = SRWLOCK_INIT; /* live edits come from other threads (the live pipe, mods' hooks) */

/* The Local Stats mod: the stats file the launcher's editor and the in-game editor save into. */
#define LOCAL_STATS_ID "zz.local.stats"
#define EDITOR_ID "hellforge.editor"

static int IsLocalStats(const HfMod* mod) { return mod != NULL && !strcmp(mod->id, LOCAL_STATS_ID); }

/* Whose live edit of a field is on top: the editors', then Lua mods', then code mods'. */
enum { RANK_CODE, RANK_LUA, RANK_EDITOR };
static int ModRank(const HfMod* mod)
{
    if (mod == NULL) return RANK_CODE;
    if (IsLocalStats(mod) || !strcmp(mod->id, EDITOR_ID)) return RANK_EDITOR;
    return mod->lua ? RANK_LUA : RANK_CODE;
}

static uintptr_t ComponentRecordOf(const char* name, uint64_t resource, uint32_t* stride, int shared);
/* The record lookup edits use; the native fixture replaces it. */
static uintptr_t (*g_fieldRecord)(const char* name, uint64_t resource, uint32_t* stride, int shared) = ComponentRecordOf;

static uint32_t TypeSize(uint32_t type)
{
    switch (type)
    {
    case HF_TYPE_U8: case HF_TYPE_BYTES: return 1;
    case HF_TYPE_U16: return 2;
    case HF_TYPE_U32: case HF_TYPE_I32: case HF_TYPE_F32: return 4;
    case HF_TYPE_U64: return 8;
    default: return 0;
    }
}

static const char* TypeName(uint32_t type)
{
    static const char* const names[] = {"?", "u8", "u16", "u32", "i32", "f32", "u64", "bytes"};
    return type < sizeof names / sizeof names[0] ? names[type] : "?";
}

static const SdkComponent* SdkComponentNamed(const char* component)
{
    for (size_t i = 0; component != NULL && i < sizeof kSdkComponents / sizeof kSdkComponents[0]; i++)
        if (!strcmp(kSdkComponents[i].component, component) || !strcmp(kSdkComponents[i].name, component)) return &kSdkComponents[i];
    return NULL;
}

static const SdkField* SdkFieldNamed(const SdkComponent* c, const char* name)
{
    for (int i = 0; c != NULL && i < c->count; i++) if (!strcmp(c->fields[i].name, name)) return &c->fields[i];
    return NULL;
}

/* One element of an array field, as a field of its own ("fire_rate[1]"): made once and kept. */
typedef struct ElementField { SdkField field; char name[80]; } ElementField;
static ElementField g_elementFields[512];
static int g_elementFieldCount;

static const SdkField* ElementOf(const SdkField* array, uint16_t offset, uint16_t size)
{
    for (int i = 0; i < g_elementFieldCount; i++)
        if (g_elementFields[i].field.offset == offset && g_elementFields[i].field.size == size && !strncmp(g_elementFields[i].name, array->name, strlen(array->name)) &&
            g_elementFields[i].name[strlen(array->name)] == '[') return &g_elementFields[i].field;
    if (g_elementFieldCount >= (int)(sizeof g_elementFields / sizeof g_elementFields[0])) return NULL;
    ElementField* e = &g_elementFields[g_elementFieldCount++];
    snprintf(e->name, sizeof e->name, "%s[%u]", array->name, (unsigned)((offset - array->offset) / size));
    e->field = *array;
    e->field.name = e->name;
    e->field.offset = offset;
    e->field.size = size;
    return &e->field;
}

/* A Lua mod's field the mappings do not name (lua_bridge.c): HD2Runtime's catalogues review far more of each record
 * than HellForge's mappings, so a Lua mod may name any single value inside its record, aligned. Made once and kept (an
 * edit points at its field). A change to a component record is taken when the entity is next made, to a settings row
 * the next time the game reads it, as the mapped fields of those records mostly say. */
#define RAW_FIELD_MAX 16384
typedef struct RawField { SdkField field; const SdkComponent* component; char name[16]; } RawField;
static RawField* g_rawFields[RAW_FIELD_MAX];
static int g_rawFieldCount;
static SRWLOCK g_rawFieldsLock = SRWLOCK_INIT;

static const SdkField* RawFieldOf(const SdkComponent* c, uint16_t offset, uint16_t size, uint32_t type)
{
    if (type == HF_TYPE_BYTES || size == 0 || size != TypeSize(type) || (uint32_t)offset + size > c->stride || offset % (size < 4 ? size : 4)) return NULL;
    const SdkField* found = NULL;
    AcquireSRWLockExclusive(&g_rawFieldsLock);
    for (int i = 0; i < g_rawFieldCount && found == NULL; i++)
        if (g_rawFields[i]->component == c && g_rawFields[i]->field.offset == offset && g_rawFields[i]->field.size == size && g_rawFields[i]->field.type == type)
            found = &g_rawFields[i]->field;
    RawField* r = found == NULL && g_rawFieldCount < RAW_FIELD_MAX ? (RawField*)calloc(1, sizeof *r) : NULL;
    if (r != NULL)
    {
        snprintf(r->name, sizeof r->name, "+0x%X", (unsigned)offset);
        r->component = c;
        r->field = (SdkField){r->name, offset, size, type, c->table ? HF_APPLIES_NOW : HF_APPLIES_RESPAWN, REF_NONE};
        g_rawFields[g_rawFieldCount++] = r;
        found = &r->field;
    }
    ReleaseSRWLockExclusive(&g_rawFieldsLock);
    return found;
}

/* The reviewed field of c a mod's field names (component, layout, offset, size and type), or one element of an array
 * field (HF_FIELD_AT), or NULL. */
static const SdkField* ReviewedField(const SdkComponent* c, const HfField* f)
{
    for (int i = 0; i < c->count; i++)
        if (c->fields[i].offset == f->offset && c->fields[i].size == f->size && c->fields[i].type == f->type) return &c->fields[i];
    for (int i = 0; i < c->count; i++)
    {
        const SdkField* a = &c->fields[i];
        uint32_t element = TypeSize(a->type);
        if (a->type == f->type && a->type != HF_TYPE_BYTES && f->size == element && a->size > element && f->offset >= a->offset &&
            f->offset + f->size <= a->offset + a->size && (f->offset - a->offset) % element == 0)
        {
            const SdkField* one = ElementOf(a, f->offset, f->size);
            if (one != NULL) return one;
        }
    }
    return NULL;
}

/* A mod's field, if it is one of the loader's: component, layout, offset, size and type all as reviewed. One element of
 * an array field is a field too (HF_FIELD_AT; the live editor's stats fields). A Lua mod's may be any value in the
 * record (RawFieldOf). */
static const SdkField* ResolveField(HfMod* mod, const char* what, const HfField* f, const SdkComponent** component)
{
    const SdkComponent* c = f != NULL ? SdkComponentNamed(f->component) : NULL;
    if (f == NULL || f->component == NULL) { ApiLog(mod, "%s: no field", what); return NULL; }
    if (c == NULL) { ApiLog(mod, "%s: line %u: HellForge has no mappings for %s", what, f->line, f->component); return NULL; }
    if (f->layout != c->layout)
    {
        ApiLog(mod, "%s: line %u: the mod was built against layout %u of %s; HellForge has layout %u (rebuild it with this SDK)",
            what, f->line, f->layout, c->name, c->layout);
        return NULL;
    }
    const SdkField* found = ReviewedField(c, f);
    if (found == NULL && mod != NULL && mod->lua) found = RawFieldOf(c, f->offset, f->size, f->type);
    if (found != NULL)
    {
        *component = c;
        return found;
    }
    ApiLog(mod, "%s: line %u: %s +0x%X (%u bytes, %s) is not a reviewed field of layout %u", what, f->line, c->name, f->offset,
        f->size, TypeName(f->type), c->layout);
    return NULL;
}

/* The largest value of an integer field type (u8, u16, u32; else i32's). */
static double TypeMax(uint32_t type)
{
    switch (type)
    {
    case HF_TYPE_U8: return 255.0;
    case HF_TYPE_U16: return 65535.0;
    case HF_TYPE_U32: return 4294967295.0;
    default: return 2147483647.0;
    }
}

/* A number's bytes for a single-value field of f's type (but u64), or 1 when the type cannot hold it. */
static int NumberBytes(const SdkField* f, double n, uint8_t* out)
{
    double max = TypeMax(f->type);
    double min = f->type == HF_TYPE_I32 ? -2147483648.0 : 0.0;
    if (f->type == HF_TYPE_F32)
    {
        float value = (float)n;
        if (!isfinite(n) || !isfinite(value)) return 1;
        memcpy(out, &value, 4);
        return 0;
    }
    if (!(n >= min && n <= max) || n != floor(n)) return 1;
    if (f->type == HF_TYPE_I32) { int32_t value = (int32_t)n; memcpy(out, &value, 4); }
    else { uint32_t value = (uint32_t)n; memcpy(out, &value, f->size); }
    return 0;
}

/* The value's bytes for a single-value field, or 1 (logged): its type must be the field's, and fit it. */
static int ValueBytes(HfMod* mod, const char* what, uint32_t line, const SdkComponent* c, const SdkField* f, const HfValue* v, uint8_t* out)
{
    memset(out, 0, 8);
    if (v == NULL) { ApiLog(mod, "%s: line %u: no value", what, line); return 1; }
    if (f->type == HF_TYPE_BYTES || f->size != TypeSize(f->type))
    {
        ApiLog(mod, "%s: line %u: %s.%s is %u bytes; copy it instead (only single values are set)", what, line, c->name, f->name, f->size);
        return 1;
    }
    if (v->type != f->type)
    {
        ApiLog(mod, "%s: line %u: %s.%s is %s; the value is %s", what, line, c->name, f->name, TypeName(f->type), TypeName(v->type));
        return 1;
    }
    if (f->type == HF_TYPE_U64) { memcpy(out, &v->u64, 8); return 0; }
    if (!NumberBytes(f, v->number, out)) return 0;
    ApiLog(mod, "%s: line %u: %s.%s (%s) cannot hold %g", what, line, c->name, f->name, TypeName(f->type), v->number);
    return 1;
}

static int TableKnown(const char* component)
{
    int extra = StatsExtraKind(component);
    return StatsTableKind(component) || (extra >= 0 && extra < (int)STATS_EXTRA_COUNT - 1);
}

/* The projectile, damage or stratagem table the game reads now and its row count. */
static uintptr_t TypedTable(uintptr_t game, int projectile, int damage, uint64_t* count)
{
    if (projectile)
    {
        uintptr_t table = ProjectileTable(game);
        *count = g_projectileTable ? PRJ_VANILLA + PRJ_MAX_NEW : PRJ_VANILLA;
        return table;
    }
    if (damage)
    {
        uintptr_t table = g_damageTable ? (uintptr_t)g_damageTable : game + DMG_TABLE;
        *count = g_damageTable ? DMG_VANILLA + DMG_MAX_NEW : DMG_VANILLA;
        return table;
    }
    uintptr_t table = g_stratagemState == 1 && g_stratagemTable ? (uintptr_t)g_stratagemTable : game + STR_TABLE;
    *count = g_stratagemState == 1 ? STR_VANILLA + g_stratagemCount : STR_VANILLA;
    return table;
}

/* A projectile, damage or stratagem row by its type (the row's first u32). */
static uintptr_t TypedRowOf(uintptr_t game, int projectile, int damage, uint64_t id, int* ready)
{
    if (game == 0 || ((projectile || damage) ? !g_fired[HF_EVENT_PROJECTILES_READY] : g_stratagemState == 0)) { *ready = 0; return 0; }
    uint64_t count = 0;
    uintptr_t table = TypedTable(game, projectile, damage, &count);
    uintptr_t row = 0;
    uint32_t type = 0;
    if (id == 0 || id >= count || SafeRead(table + id * 8, &row, 8) || row == 0 || SafeRead(row, &type, 4) || type != id) return 0;
    return row;
}

static uintptr_t BoosterRowOf(uint64_t id, int* ready)
{
    uintptr_t table = BoosterTable();
    *ready = table != 0 || g_boostersDone;
    return table && id >= 1 && id < BOOSTER_ROWS ? table + (uintptr_t)id * BOOSTER_ROW : 0;
}

static uintptr_t MeleeRowOf(uint64_t id, int* ready)
{
    uintptr_t row = MeleeVolume(id);
    *ready = row != 0 || g_meleeDone;
    return row;
}

/* level 11's moved table, else (none added, or its move refused) the game's own: director.c */
static uintptr_t DifficultyRowOf(uint64_t id, int* ready)
{
    if (g_difficultyState == 0) { *ready = 0; return 0; }
    if (g_difficultyState == 1 && g_difficultyTable != NULL) return id <= 11 ? ApiDifficultyRow((uint32_t)id) : 0;
    return DifficultyVanillaRow(id, ready);
}

/* A row of the stats mods' other tables (stats-tables.c). */
static uintptr_t ExtraRowOf(const char* component, uint64_t id, int* ready)
{
    int kind = StatsExtraKind(component);
    if (kind < 0 || kind >= (int)STATS_EXTRA_COUNT - 1) return 0;
    if (!g_statsAllocations[kind]) { *ready = g_statsExtraDone[kind]; return 0; }
    StatEdit probe = {0};
    snprintf(probe.kind, sizeof probe.kind, "%s", component);
    probe.record = (int64_t)id;
    probe.offset = 4;
    uintptr_t at = StatsExtraRow(&probe);
    return at ? at - 4 : 0;
}

/* A settings table's row by its id, through the table the game reads now (HellForge's moved copy, else the game's): the
 * row, or 0 with *ready 0 while the table is not loaded yet, or 0 with *ready 1 when there is no such row. */
static uintptr_t TableRowOf(const char* component, uint64_t id, int* ready)
{
    *ready = 1;
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    int projectile = !strcmp(component, "ProjectileSettings"), damage = !strcmp(component, "DamageInfo");
    if (projectile || damage || !strcmp(component, "StratagemDefinition")) return TypedRowOf(game, projectile, damage, id, ready);
    if (!strcmp(component, "BoosterSettings")) return BoosterRowOf(id, ready);
    if (!strncmp(component, "Passive", 7)) return PerkRow(component, id, ready);
    if (!strcmp(component, "MeleeHitVolume")) return MeleeRowOf(id, ready);
    if (!strcmp(component, "DifficultySettings")) return DifficultyRowOf(id, ready);
    if (DirectorKind(component)) return DirectorRow(component, id, ready);
    return ExtraRowOf(component, id, ready);
}
/* The table row lookup edits use; the native fixture replaces it. */
static uintptr_t (*g_tableRow)(const char* component, uint64_t id, int* ready) = TableRowOf;

/* A stats mod's own copy of an entity's record ("own": true), which the entity now reads instead of the vanilla one. */
static uintptr_t StatsOwnRecordOf(const char* component, uint64_t resource)
{
    for (int i = 0; i < g_statCount; i++)
        if (g_stats[i].applied && g_stats[i].own && g_stats[i].resource == resource && !strcmp(g_stats[i].component, component))
            return g_stats[i].address - g_stats[i].offset;
    return 0;
}

/* An entity a mod added (not refused), or NULL. */
static Weapon* ModEntity(uint64_t entity)
{
    Weapon* w = NULL;
    for (int i = 0; i < g_weaponCount; i++) if (g_weapons[i].entity == entity && g_weapons[i].state != -1) w = &g_weapons[i];
    return w;
}

/* EditTarget for a settings table: 2 when HellForge reaches its rows, else -1 (logged). */
static int TableTarget(HfMod* mod, const char* what, const SdkComponent* c)
{
    if (TableKnown(c->component)) return 2;
    ApiLog(mod, "%s: HellForge cannot reach %s rows yet", what, c->component);
    return -1;
}

/* Whether the target's record can be edited: 0 a mod's entity (from hellforge_mod_init it gets a record of its own in
 * the component; later it must have one), 1 a vanilla entity (checked when the data loads), 2 a table row (by its id),
 * -1 no (logged). */
static int EditTarget(HfMod* mod, const char* what, uint64_t entity, const SdkComponent* c)
{
    if (c->table) return TableTarget(mod, what, c);
    const Component* component = ComponentNamed(c->component);
    if (component == NULL) { ApiLog(mod, "%s: this session has no %s table", what, c->component); return -1; }
    if (entity == 0) { ApiLog(mod, "%s: no entity", what); return -1; }
    Weapon* w = ModEntity(entity);
    if (w == NULL) return 1;
    for (int i = 0; i < w->ownCount; i++) if (w->own[i] == component->index) return 0;
    if (component->index == REG_BEHAVIOUR && w->behaviour != 0) return 0;
    if (w->state != 0 || g_fired[HF_EVENT_ENTITIES_READY] || w->ownCount >= REG_MAX_OWN || SlotsOf(component->index) == 0)
    {
        ApiLog(mod, "%s: %s has no %s record of its own (edit it from hellforge_mod_init, or name the field in its definition)",
            what, w->name, c->name);
        return -1;
    }
    w->own[w->ownCount] = component->index;
    w->ownStride[w->ownCount++] = component->stride;
    return 0;
}

/* The edits queued before it (the first `before`) that write bytes [start, end) of the same record: another mod's. */
static void ReportEditConflicts(const FieldEdit* e, int before, uint32_t start, uint32_t end)
{
    for (int i = 0; i < before; i++)
    {
        const FieldEdit* o = &g_edits[i];
        if (o == e || o->mod == e->mod || o->kind == EDIT_TAG || o->entity != e->entity || o->component != e->component) continue;
        if (o->field->offset >= end || o->field->offset + o->field->size <= start) continue;
        g_fieldConflicts++;
        ApiLog(o->mod, "conflict: %s (line %u) also changes %016llx %s.%s; its edit applies after this mod's", e->mod->id, e->line,
            (unsigned long long)e->entity, e->component->name, e->field->name);
        ApiLog(e->mod, "conflict: %s (line %u) also changes %016llx %s.%s; this mod's edit applies after it", o->mod->id, o->line,
            (unsigned long long)e->entity, e->component->name, o->field->name);
        Note("conflict: %s and %s both change %016llx %s.%s; %s's applies (load order)", o->mod->id, e->mod->id,
            (unsigned long long)e->entity, e->component->name, e->field->name, e->mod->id);
    }
}

/* Whether stats edit st writes bytes [start, end) of the record or row e edits (but the mod's own stats file, and the
 * editors' save file under a live edit). */
static int StatOverlaps(const StatEdit* st, const FieldEdit* e, uint32_t start, uint32_t end)
{
    if (st->mod != NULL && !strcmp(st->mod->id, e->mod->id)) return 0; /* the live editor and its own stats file */
    if (e->live && st->mod != NULL && !strcmp(st->mod->id, "zz.local.stats")) return 0; /* the editors' save file */
    if (e->table ? (strcmp(st->kind, e->component->component) || st->record != (int64_t)e->entity) :
        (strcmp(st->kind, "component") || strcmp(st->component, e->component->component) || st->resource != e->entity)) return 0;
    return !(st->offset >= end || st->offset + 4 <= start);
}

/* Reports an edit that writes bytes another mod's edit (or a stats mod's) writes too: the later one stays. */
static void ReportConflicts(const FieldEdit* e, int before)
{
    if (e->kind == EDIT_TAG) return; /* tag lists compose: each edit adds or removes one tag */
    uint32_t start = e->field->offset, end = start + e->field->size;
    ReportEditConflicts(e, before, start, end);
    for (int i = 0; i < g_statCount; i++)
    {
        const StatEdit* st = &g_stats[i];
        if (!StatOverlaps(st, e, start, end)) continue;
        g_fieldConflicts++;
        ApiLog(e->mod, "conflict: stats mod %s also sets %016llx %s.%s; this mod's edit applies after the stats", st->mod ? st->mod->id : "?",
            (unsigned long long)e->entity, e->component->name, e->field->name);
        Note("conflict: stats mod %s and %s both change %016llx %s.%s; %s's applies", st->mod ? st->mod->id : "?", e->mod->id,
            (unsigned long long)e->entity, e->component->name, e->field->name, e->mod->id);
    }
}

static void ApplyEdit(FieldEdit* e);

/* Room for one more edit (under g_fieldsLock): 1, or 0 at the bound or out of memory. */
static int EditRoom(void)
{
    if (g_editCount < g_editCapacity) return 1;
    int capacity = g_editCapacity ? g_editCapacity * 2 : 1024;
    FieldEdit* grown = capacity <= FIELD_MAX_EDITS ? (FieldEdit*)realloc(g_edits, (size_t)capacity * sizeof *grown) : NULL;
    if (grown == NULL) return 0;
    g_edits = grown;
    g_editCapacity = capacity;
    return 1;
}

static int QueueEdit(FieldEdit edit)
{
    AcquireSRWLockExclusive(&g_fieldsLock);
    int refused = !EditRoom();
    if (refused) ApiLog(edit.mod, "deferred edits: mods queued %d already", FIELD_MAX_EDITS);
    else if (g_modsSettingUp)
    {
        /* while the mods set up (in id order): in load order among the queued edits; reported and applied once all have
         * (FinishSetupEdits) */
        int at = g_editCount;
        while (at > 0 && ModLoadIndex(g_edits[at - 1].mod) > ModLoadIndex(edit.mod)) at--;
        memmove(&g_edits[at + 1], &g_edits[at], (size_t)(g_editCount - at) * sizeof g_edits[0]);
        g_edits[at] = edit;
        g_editCount++;
    }
    else
    {
        ReportConflicts(&edit, g_editCount);
        g_edits[g_editCount] = edit;
        if (edit.table || g_fired[HF_EVENT_ENTITIES_READY]) ApplyEdit(&g_edits[g_editCount]);
        g_editCount++;
    }
    ReleaseSRWLockExclusive(&g_fieldsLock);
    return refused;
}

/* The edits the mods queued while they set up, now in load order: their conflicts reported (the later one applies), the
 * table rows written, as QueueEdit does for each edit after the set-up. */
static void FinishSetupEdits(void)
{
    AcquireSRWLockExclusive(&g_fieldsLock);
    for (int i = 0; i < g_editCount; i++)
    {
        ReportConflicts(&g_edits[i], i);
        if (g_edits[i].table || g_fired[HF_EVENT_ENTITIES_READY]) ApplyEdit(&g_edits[i]);
    }
    ReleaseSRWLockExclusive(&g_fieldsLock);
}

static int ApiCopyField(HfMod* mod, HfRef entity, HfRef from, const HfField* field)
{
    const SdkComponent* c = NULL;
    const SdkField* f = ResolveField(mod, "copy_field", field, &c);
    if (f == NULL) return 1;
    if (from == 0) { ApiLog(mod, "copy_field: line %u: no entity to copy %s.%s from", field->line, c->name, f->name); return 1; }
    int target = EditTarget(mod, "copy_field", entity, c);
    if (target < 0) return 1;
    FieldEdit e = {mod, EDIT_COPY, entity, from, c, f};
    e.line = field->line;
    e.vanilla = target == 1;
    e.table = target == 2;
    return QueueEdit(e);
}

static int ApiSetField(HfMod* mod, HfRef entity, const HfField* field, const HfValue* value)
{
    const SdkComponent* c = NULL;
    const SdkField* f = ResolveField(mod, "set_field", field, &c);
    FieldEdit e = {mod, EDIT_SET, entity, 0, c, f};
    if (f == NULL || ValueBytes(mod, "set_field", field->line, c, f, value, e.value)) return 1;
    int target = EditTarget(mod, "set_field", entity, c);
    if (target < 0) return 1;
    e.line = field->line;
    e.vanilla = target == 1;
    e.table = target == 2;
    return QueueEdit(e);
}

/* Tags and mounts are fields of the mappings too: TagComponentData.tags (a list ending in 0) and MountComponentData's
 * slots. */
static int ApiSetTag(HfMod* mod, HfRef entity, uint64_t tag, int present)
{
    const SdkComponent* c = SdkComponentNamed("Tag");
    const SdkField* f = SdkFieldNamed(c, "tags");
    if (f == NULL || tag == 0) { ApiLog(mod, "set_tag: %s", tag == 0 ? "no tag" : "HellForge has no mappings for TagComponentData.tags"); return 1; }
    int target = EditTarget(mod, "set_tag", entity, c);
    if (target < 0) return 1;
    FieldEdit e = {mod, EDIT_TAG, entity, 0, c, f};
    e.vanilla = target == 1;
    memcpy(e.value, &tag, 8);
    e.present = present != 0;
    return QueueEdit(e);
}

static int ApiMount(HfMod* mod, HfRef entity, uint32_t slot, HfRef carried)
{
    const SdkComponent* c = SdkComponentNamed("Mount");
    char name[32];
    snprintf(name, sizeof name, "slot%u_entity", slot);
    const SdkField* f = SdkFieldNamed(c, name);
    if (f == NULL || f->type != HF_TYPE_U64) { ApiLog(mod, "mount: HellForge has no mappings for MountComponentData slot %u", slot); return 1; }
    if (carried == 0) { ApiLog(mod, "mount: slot %u: nothing to carry", slot); return 1; }
    int target = EditTarget(mod, "mount", entity, c);
    if (target < 0) return 1;
    FieldEdit e = {mod, EDIT_SET, entity, 0, c, f};
    e.vanilla = target == 1;
    memcpy(e.value, &carried, 8);
    return QueueEdit(e);
}

static void EditFailed(FieldEdit* e, const char* why)
{
    if (e->failed) return;
    e->failed = 1;
    ApiLog(e->mod, "%s edit of %016llx %s.%s (line %u) not applied: %s", e->live ? "live" : "deferred", (unsigned long long)e->entity,
        e->component->name, e->field->name, e->line, why);
    if (!e->live) /* a live edit's refusal goes back to its caller (and the mod's log); a slider would flood modloader.log */
        Note("%s: deferred edit of %016llx %s.%s not applied: %s", e->mod->id, (unsigned long long)e->entity, e->component->name, e->field->name, why);
}

/* The field's new bytes are worked out in a copy and written with ApiWrite: the game's own entity data (a vanilla
 * entity's record) is read-only, HellForge's copies of tables (a mod entity's own records) are not. */
/* The record an edit changes (or copies from, with `from`): a table row by id, a stats mod's own copy, the entity's own
 * record, or (shared) the record it shares. NULL with *waiting 1 while its table is not loaded. */
static uint8_t* EditRecord(const FieldEdit* e, uint64_t id, int from, int* waiting)
{
    *waiting = 0;
    if (e->table)
    {
        int ready = 1;
        uint8_t* row = (uint8_t*)g_tableRow(e->component->component, id, &ready);
        *waiting = !ready;
        return row;
    }
    uintptr_t own = from ? 0 : StatsOwnRecordOf(e->component->component, id);
    if (own) return (uint8_t*)own;
    uint32_t stride = 0;
    uint8_t* record = (uint8_t*)g_fieldRecord(e->component->component, id, &stride, from || e->shared);
    return record != NULL && stride == e->component->stride ? record : NULL;
}

/* Whether the bytes at `at` hold the value a Local Stats edit wrote at boot (the editors' saved change): a Lua or code
 * mod's live edit stays under it while they do. */
static int SavedEditorValue(uintptr_t at, uint32_t size)
{
    if (size != 1 && size != 4) return 0;
    for (int i = 0; i < g_statCount; i++)
        if (g_stats[i].applied && g_stats[i].address == at && IsLocalStats(g_stats[i].mod))
        {
            uint32_t now = 0;
            return !SafeRead(at, &now, size) && memcmp(&now, &g_stats[i].value, size) == 0;
        }
    return 0;
}

/* Why an edit has no record to change (EditRecord gave none). */
static const char* NoRecordReason(const FieldEdit* e)
{
    if (e->table) return "its table has no such row";
    return e->vanilla ? "it has no such record, or shares it with other entities (stats mods change shared records)" : "the entity has no record of its own";
}

/* EDIT_COPY's bytes: the entity's it copies from; 1 if refused (logged). */
static int CopiedBytes(FieldEdit* e, uint8_t* bytes)
{
    int waiting = 0;
    const uint8_t* source = EditRecord(e, e->from, 1, &waiting);
    if (source == NULL) { EditFailed(e, "the entity it copies from has no such record"); return 1; }
    memcpy(bytes, source + e->field->offset, e->field->size);
    return 0;
}

/* EDIT_SET's bytes: its value, when what it names may be named; 1 if not applied (logged, or waiting for packages). */
static int SetBytes(FieldEdit* e, const uint8_t* at, uint8_t* bytes)
{
    /* a reference is checked on the game's own rows and records (a mod mounting its own entities on its own
     * vehicle names what it registered) */
    if (e->field->ref != REF_NONE && !((e->field->ref == REF_MOUNT || e->field->ref == REF_PICKUP) && !e->vanilla) && RefEdit(e, at) != REF_OK) return 1;
    uint64_t named = 0;
    if (e->field->size == 8) memcpy(&named, e->value, 8);
    if (named != 0 && RefusedEntity(named))
    {
        EditFailed(e, "it names an entity a mod added that was not registered (see the mod's log); the field keeps its value");
        return 1;
    }
    memcpy(bytes, e->value, e->field->size);
    return 0;
}

/* EDIT_TAG's bytes: the tag list (ending in 0) with the tag added or removed; 1 if refused (logged). */
static int TaggedBytes(FieldEdit* e, uint8_t* bytes)
{
    uint32_t count = e->field->size / 8;
    uint64_t tag, current;
    memcpy(&tag, e->value, 8);
    uint32_t n = 0, found = count;
    for (; n < count; n++)
    {
        memcpy(&current, bytes + n * 8, 8);
        if (current == 0) break;
        if (current == tag) found = n;
    }
    if (e->present && found == count)
    {
        if (n == count) { EditFailed(e, "the tag list is full"); return 1; }
        memcpy(bytes + n * 8, &tag, 8);
    }
    else if (!e->present && found != count)
    {
        memmove(bytes + found * 8, bytes + (found + 1) * 8, (size_t)(count - found - 1) * 8);
        memset(bytes + (count - 1) * 8, 0, 8);
    }
    return 0;
}

/* The field's new bytes, worked out in `bytes` (its bytes now); 1 if not applied. */
static int EditBytes(FieldEdit* e, const uint8_t* at, uint8_t* bytes)
{
    if (e->kind == EDIT_COPY) return CopiedBytes(e, bytes);
    if (e->kind == EDIT_SET) return SetBytes(e, at, bytes);
    return TaggedBytes(e, bytes);
}

/* After a write, what follows the record's change elsewhere: was holds the field's first wasSize bytes from before. */
static void EditWritten(const FieldEdit* e, const uint8_t* record, const uint8_t* was, size_t wasSize, const uint8_t* bytes)
{
    /* the game's own difficulty table (no level 11): every copy that held the same bytes */
    if (e->table && !strcmp(e->component->component, "DifficultySettings") && e->field->size <= wasSize)
        DifficultyMirror((uintptr_t)record, e->field->offset, was, bytes, e->field->size);
    /* a weapon's projectile changed live (the editor's swap): this machine's own, for co-op's shots (shots.c) */
    if (!e->table && e->field->size == 4 && ShotsSlotOf(e->component->component, e->field->offset) >= 0)
    {
        uint32_t now;
        memcpy(&now, bytes, 4);
        uint32_t before;
        memcpy(&before, was, 4);
        ShotsNoteLocal(e->entity, ShotsSlotOf(e->component->component, e->field->offset), before, now);
    }
}

static void ApplyEdit(FieldEdit* e)
{
    int waiting = 0;
    uint8_t* record = EditRecord(e, e->entity, 0, &waiting);
    if (waiting) { e->waiting = 1; g_editsWaiting = 1; return; }
    e->waiting = 0;
    if (record == NULL) { EditFailed(e, NoRecordReason(e)); return; }
    uint8_t* at = record + e->field->offset;
    e->address = (uintptr_t)at;
    uint8_t bytes[1024], was[8];
    if (e->field->size > sizeof bytes) { EditFailed(e, "the field is larger than HellForge edits"); return; }
    memcpy(bytes, at, e->field->size);
    memcpy(was, at, e->field->size < sizeof was ? e->field->size : sizeof was);
    /* under a value Local Stats saved (not written; its original is taken when it does apply) */
    if (e->live && ModRank(e->mod) < RANK_EDITOR && SavedEditorValue((uintptr_t)at, e->field->size)) { e->failed = 0; return; }
    if (e->live && !e->hasOriginal && e->field->size <= sizeof e->original) { memcpy(e->original, at, e->field->size); e->hasOriginal = 1; }
    if (EditBytes(e, at, bytes)) return;
    /* the spawn director's rows: a composition's squad within the queue's capacity, its ranges in order (director.c) */
    const char* refused = e->table ? DirectorRefuses(e->component->component, (uintptr_t)record, e->field->offset, bytes, e->field->size) : NULL;
    if (refused != NULL) { EditFailed(e, refused); return; }
    if (memcmp(bytes, at, e->field->size) != 0 && ApiWrite((uintptr_t)at, bytes, e->field->size)) { EditFailed(e, "its record could not be written"); return; }
    EditWritten(e, record, was, sizeof was, bytes);
    e->failed = 0;
}

/* A mod stratagem that names no package (HfStratagem.package 0) but a payload loads the package of the entity it drops:
 * the payload's LoadoutPackage record (a mod entity's own, else its model's or base's), last payload entity first (a
 * Pelican drop's vehicle). Done once the entity data and the stratagem rows both exist, again after a reload. */
static int g_packagesInferred;

/* For an entity a mod added: the entity whose model it takes, else its base (0 for any other). */
static uint64_t ModelOrBase(uint64_t entity)
{
    uint64_t source = 0;
    for (int w = 0; w < g_weaponCount; w++)
        if (g_weapons[w].entity == entity) source = g_weapons[w].modelFrom ? g_weapons[w].modelFrom : g_weapons[w].baseEntity;
    return source;
}

/* The package of a stratagem's payload (its last payload entity first), or 0; *from: the entity whose record names it. */
static uint64_t PayloadPackage(const Stratagem* st, const SdkComponent* c, const SdkField* f, uint64_t* from)
{
    uint64_t package = 0;
    for (uint32_t k = st->payloadCount; k-- > 0 && package == 0;)
    {
        uint64_t entity = st->payload[k];
        if (entity == 0) continue;
        uint64_t source[2] = {entity, ModelOrBase(entity)};
        for (int n = 0; n < 2 && package == 0 && source[n] != 0; n++)
        {
            uint32_t stride = 0;
            const uint8_t* record = (const uint8_t*)g_fieldRecord(c->component, source[n], &stride, 1);
            if (record != NULL && stride == c->stride) { memcpy(&package, record + f->offset, 8); *from = source[n]; }
        }
    }
    return package;
}

static void InferStratagemPackages(void)
{
    g_packagesInferred = 1;
    const SdkComponent* c = SdkComponentNamed("LoadoutPackage");
    const SdkField* f = SdkFieldNamed(c, "package");
    if (f == NULL) return;
    for (uint32_t i = 0; i < g_stratagemCount; i++)
    {
        Stratagem* st = &g_stratagems[i];
        if (st->row == NULL || st->package != 0 || st->payloadCount == 0) continue;
        uint64_t from = 0;
        uint64_t package = PayloadPackage(st, c, f, &from);
        if (package == 0)
        {
            ApiLog(st->mod, "%s: no package found for its payload; it keeps its base's", st->name);
            continue;
        }
        memcpy(st->row + 0xA8, &package, 8);
        ApiLog(st->mod, "%s loads package %016llx (its payload's, from %016llx)", st->name, (unsigned long long)package, (unsigned long long)from);
    }
}

/* Every deferred edit, in the order mods queued them: each time the game loads its entity data, before the mods hear of it.
 * Entity records are fresh after a load, so live edits take their originals again; table rows are not reloaded. */
static void ApplyFieldEdits(void)
{
    AcquireSRWLockExclusive(&g_fieldsLock);
    g_packagesInferred = 0;
    for (int i = 0; i < g_editCount; i++) { g_edits[i].failed = 0; if (!g_edits[i].table) g_edits[i].hasOriginal = 0; }
    for (int i = 0; i < g_editCount; i++) ApplyEdit(&g_edits[i]);
    if (g_editCount > 0)
    {
        int failed = 0, waiting = 0;
        for (int i = 0; i < g_editCount; i++) { failed += g_edits[i].failed; waiting += g_edits[i].waiting; }
        Note("deferred edits: %d applied, %d refused, %d waiting for their tables", g_editCount - failed - waiting, failed, waiting);
    }
    ReleaseSRWLockExclusive(&g_fieldsLock);
}

/* Edits whose tables have loaded since (the watch loop calls this while any waits). */
static void ApplyWaitingEdits(void)
{
    AcquireSRWLockExclusive(&g_fieldsLock);
    g_editsWaiting = 0;
    for (int i = 0; i < g_editCount; i++) if (g_edits[i].waiting) ApplyEdit(&g_edits[i]);
    ReleaseSRWLockExclusive(&g_fieldsLock);
}

static uint32_t DefinedBehaviour(HfMod* mod, const HfBehaviour* def)
{
    for (int i = 0; i < g_behaviourDefCount; i++) if (g_behaviourDefs[i].def == def) return g_behaviourDefs[i].id;
    if (g_behaviourDefCount >= FIELD_MAX_DEFS) { ApiLog(mod, "register_entity: too many behaviours"); return 0; }
    uint32_t id = ApiAddBehaviour(mod, def);
    if (id != 0) g_behaviourDefs[g_behaviourDefCount++] = (BehaviourDefinition){def, id};
    return id;
}

/* What a definition block changes: HfEntityDef's and HfEditDef's lists. */
typedef struct Changes
{
    const HfCopy* copy;
    const HfSet* set;
    const uint64_t* addTags;
    const uint64_t* removeTags;
    const HfMountDef* mount;
} Changes;

static HfRef ApiRegisterEntity(HfMod* mod, const HfEntityDef* def);

/* A component the block changes, into its own list once: 0 when the list is full. */
static int OwnComponent(const char** own, int* ownCount, const char* name)
{
    int seen = 0;
    for (int k = 0; k < *ownCount; k++) seen |= !strcmp(own[k], name);
    if (!seen && *ownCount < REG_MAX_OWN) own[(*ownCount)++] = name;
    else if (!seen) return 0;
    return 1;
}

/* The block's copies: 1 if all are accepted. */
static int CheckCopies(HfMod* mod, const char* what, const HfCopy* copy, const char** own, int* ownCount)
{
    int ok = 1;
    const SdkComponent* c = NULL;
    for (; copy != NULL && copy->from != 0; copy++)
    {
        if (ResolveField(mod, what, &copy->field, &c) == NULL) ok = 0;
        else if (!OwnComponent(own, ownCount, c->component)) ok = 0;
    }
    return ok;
}

/* The block's values: 1 if all are accepted. */
static int CheckSets(HfMod* mod, const char* what, const HfSet* set, const char** own, int* ownCount)
{
    int ok = 1;
    const SdkComponent* c = NULL;
    for (; set != NULL && set->field.component != NULL; set++)
    {
        uint8_t bytes[8];
        const SdkField* f = ResolveField(mod, what, &set->field, &c);
        if (f == NULL || ValueBytes(mod, what, set->field.line, c, f, &set->value, bytes)) ok = 0;
        else if (!OwnComponent(own, ownCount, c->component)) ok = 0;
    }
    return ok;
}

/* The block's tags: 1 if it has none, or they are accepted. */
static int CheckTags(HfMod* mod, const char* what, const Changes* ch, const char** own, int* ownCount)
{
    if (!((ch->addTags != NULL && ch->addTags[0] != 0) || (ch->removeTags != NULL && ch->removeTags[0] != 0))) return 1;
    const SdkComponent* tag = SdkComponentNamed("Tag");
    if (SdkFieldNamed(tag, "tags") == NULL) { ApiLog(mod, "%s: HellForge has no mappings for tags", what); return 0; }
    return OwnComponent(own, ownCount, tag->component);
}

/* What mount m carries: the entity of its definition block, registered, or its resource; 0 if refused. */
static HfRef MountCarried(HfMod* mod, const HfMountDef* m)
{
    return m->entity != NULL ? ApiRegisterEntity(mod, m->entity) : m->resource;
}

/* The block's mounts (at most 8), registering what they carry while all is accepted (*ok): how many it lists. */
static int CheckMounts(HfMod* mod, const char* what, const HfMountDef* m, const char** own, int* ownCount, HfRef* carried, int* ok)
{
    int mounts = 0;
    for (; m != NULL && (m->entity != NULL || m->resource != 0); m++, mounts++)
    {
        if (mounts >= 8) { ApiLog(mod, "%s: at most 8 mounts", what); *ok = 0; break; }
        const SdkComponent* mount = SdkComponentNamed("Mount");
        char slot[32];
        snprintf(slot, sizeof slot, "slot%u_entity", m->slot);
        if (SdkFieldNamed(mount, slot) == NULL) { ApiLog(mod, "%s: HellForge has no mappings for mount slot %u", what, m->slot); *ok = 0; continue; }
        if (!OwnComponent(own, ownCount, mount->component)) *ok = 0;
        carried[mounts] = *ok ? MountCarried(mod, m) : 0;
        if (carried[mounts] == 0) *ok = 0;
    }
    return mounts;
}

/* Checks every field, value and mount of a block, registering the entities its mounts carry; fills the components the
 * block changes (for an entity's own records) and what each mount carries. 1 if all of it is accepted (each problem is
 * logged with the mod's source line). */
static int CheckChanges(HfMod* mod, const char* what, const Changes* ch, const char** own, int* ownCount, HfRef* carried, int* mounts)
{
    int ok = CheckCopies(mod, what, ch->copy, own, ownCount);
    ok &= CheckSets(mod, what, ch->set, own, ownCount);
    ok &= CheckTags(mod, what, ch, own, ownCount);
    *mounts = CheckMounts(mod, what, ch->mount, own, ownCount, carried, &ok);
    if (*ownCount >= REG_MAX_OWN && !ok) ApiLog(mod, "%s: it may change at most %d components", what, REG_MAX_OWN);
    return ok;
}

/* Queues a checked block's edits; the number refused (logged). */
static int QueueChanges(HfMod* mod, HfRef entity, const Changes* ch, const HfRef* carried, int mounts)
{
    int refused = 0;
    for (const HfCopy* copy = ch->copy; copy != NULL && copy->from != 0; copy++) refused += ApiCopyField(mod, entity, copy->from, &copy->field);
    for (const HfSet* set = ch->set; set != NULL && set->field.component != NULL; set++) refused += ApiSetField(mod, entity, &set->field, &set->value);
    for (const uint64_t* tag = ch->removeTags; tag != NULL && *tag != 0; tag++) refused += ApiSetTag(mod, entity, *tag, 0);
    for (const uint64_t* tag = ch->addTags; tag != NULL && *tag != 0; tag++) refused += ApiSetTag(mod, entity, *tag, 1);
    for (int i = 0; i < mounts; i++) refused += ApiMount(mod, entity, ch->mount[i].slot, carried[i]);
    return refused;
}

/* A block registered already (*found 1): its entity, or 0 while the entities its mounts carry are registered (logged). */
static HfRef KnownDefinition(HfMod* mod, const HfEntityDef* def, int* found)
{
    *found = 0;
    for (int i = 0; i < g_entityDefCount; i++)
        if (g_entityDefs[i].def == def)
        {
            *found = 1;
            if (g_entityDefs[i].busy) { ApiLog(mod, "register_entity(%s): its mounts carry it", def->name ? def->name : "?"); return 0; }
            return g_entityDefs[i].entity;
        }
    return 0;
}

/* The add_entity a definition block makes: its base, name, unit, own components, behaviour, model and text. */
static HfEntity EntityOfDefinition(const HfEntityDef* def, const char** own, uint32_t behaviour)
{
    HfEntity entity = {sizeof entity};
    entity.base = def->base;
    entity.name = def->name;
    entity.unit = def->unit;
    entity.own_components = own;
    entity.behaviour = behaviour;
    entity.model_from = def->model_from;
    entity.own_all = def->size >= offsetof(HfEntityDef, own_all) + sizeof def->own_all ? def->own_all : 0;
    if (def->size >= offsetof(HfEntityDef, description) + sizeof def->description)
    {
        entity.title = def->title;
        entity.designation = def->designation;
        entity.description = def->description;
    }
    return entity;
}

/* Registers a definition block: the entities its mount carries first, then the entity with a record of its own in every
 * component the block changes, then its edits. Nothing is added if any field or value is refused. */
static HfRef ApiRegisterEntity(HfMod* mod, const HfEntityDef* def)
{
    if (mod == NULL || def == NULL) return 0;
    int found;
    HfRef known = KnownDefinition(mod, def, &found);
    if (found) return known;
    if (def->size != 0 && def->size < offsetof(HfEntityDef, mount) + sizeof def->mount)
    {
        ApiLog(mod, "register_entity: its size is not an HfEntityDef's");
        return 0;
    }
    char what[224];
    snprintf(what, sizeof what, "register_entity(%s)", def->name ? def->name : "?");
    if (g_entityDefCount >= FIELD_MAX_DEFS) { ApiLog(mod, "%s: too many definitions", what); return 0; }
    EntityDefinition* d = &g_entityDefs[g_entityDefCount++];
    *d = (EntityDefinition){def, mod, 0, 1};

    /* Fields first: refuse the block before anything is added. */
    Changes ch = {def->copy, def->set, def->add_tags, def->remove_tags, def->mount};
    const char* own[REG_MAX_OWN + 1] = {0};
    int ownCount = 0, mounts = 0;
    HfRef carried[8] = {0};
    if (!CheckChanges(mod, what, &ch, own, &ownCount, carried, &mounts))
    {
        ApiLog(mod, "%s: not added (see above)", what);
        d->busy = 0;
        return 0;
    }

    uint32_t behaviour = def->behaviour != NULL ? DefinedBehaviour(mod, def->behaviour) : def->behaviour_id;
    if (def->behaviour != NULL && behaviour == 0) { d->busy = 0; return 0; }
    HfEntity entity = EntityOfDefinition(def, own, behaviour);
    d->entity = ApiAddEntity(mod, &entity);
    d->busy = 0;
    if (d->entity == 0) return 0;
    QueueChanges(mod, d->entity, &ch, carried, mounts);
    return d->entity;
}

/* Changes an existing entity from a block (HfEditDef): a vanilla one (its record must be its own) or one a mod added. */
static int ApiEditEntity(HfMod* mod, const HfEditDef* def)
{
    if (mod == NULL || def == NULL) return 1;
    if (def->size != 0 && def->size < offsetof(HfEditDef, mount) + sizeof def->mount) { ApiLog(mod, "edit_entity: its size is not an HfEditDef's"); return 1; }
    char what[64];
    snprintf(what, sizeof what, "edit_entity(%016llx)", (unsigned long long)def->entity);
    if (def->entity == 0) { ApiLog(mod, "edit_entity: no entity"); return 1; }
    Changes ch = {def->copy, def->set, def->add_tags, def->remove_tags, def->mount};
    const char* own[REG_MAX_OWN + 1] = {0};
    int ownCount = 0, mounts = 0;
    HfRef carried[8] = {0};
    if (!CheckChanges(mod, what, &ch, own, &ownCount, carried, &mounts)) { ApiLog(mod, "%s: nothing changed (see above)", what); return 1; }
    return QueueChanges(mod, def->entity, &ch, carried, mounts) ? 1 : 0;
}

/* A stratagem without editor entities of its own lists each payload entity a mod added, for the entity it copies. */
static void FillEditorEntities(Stratagem* s)
{
    if (s->editorEntities[0].entity != 0) return;
    for (uint32_t p = 0, n = 0; p < s->payloadCount && n < STR_MAX_PAYLOAD; p++)
        for (int i = 0; i < g_weaponCount; i++)
            if (g_weapons[i].plain && g_weapons[i].entity == s->payload[p] && g_weapons[i].baseEntity != 0)
                s->editorEntities[n++] = (HfEditorEntity){g_weapons[i].entity, g_weapons[i].baseEntity};
}

static HfContent StratagemContent(const Stratagem* s, uint32_t size)
{
    return (HfContent){size, HF_KIND_STRATAGEM, s->type, s->base, 0, s->mod->id, s->mod->folder, s->name,
        s->title[0] ? s->title : NULL, s->iconName[0] ? s->iconName : NULL, s->editorLike, s->editorEntities, NULL, s->category};
}

static HfContent EntityContent(const Weapon* w, uint32_t size)
{
    return (HfContent){size, HF_KIND_ENTITY, w->entity, w->baseEntity, 0, w->mod->id, w->mod->folder, w->name,
        w->title[0] ? w->title : NULL, NULL, 0, NULL, w->designation[0] ? w->designation : NULL};
}

/* SDK 7: the index-th piece of mod content: stratagems, then entities, projectiles and damage, in the order added. A
 * stratagem without editor entities of its own lists each payload entity a mod added, for the entity it copies. */
static int ApiContent(HfMod* mod, uint32_t index, HfContent* out)
{
    if (out == NULL || out->size < offsetof(HfContent, editor_entities) + sizeof out->editor_entities) return 0;
    uint32_t size = out->size;
    HfContent c = {0};
    #define GIVE() (memcpy(out, &c, size < sizeof c ? size : sizeof c), 1) /* no more than the caller's struct holds */
    if (index < g_stratagemCount)
    {
        Stratagem* s = &g_stratagems[index];
        FillEditorEntities(s);
        c = StratagemContent(s, size);
        return GIVE();
    }
    index -= g_stratagemCount;
    for (int i = 0; i < g_weaponCount; i++)
        if (g_weapons[i].plain && g_weapons[i].state != -1 && index-- == 0)
        {
            c = EntityContent(&g_weapons[i], size);
            return GIVE();
        }
    if (index < g_projectileCount)
    {
        Projectile* p = &g_projectiles[index];
        c = (HfContent){size, HF_KIND_PROJECTILE, p->type, p->base, p->damage, p->mod->id, p->mod->folder, p->name, NULL, NULL, 0, NULL};
        return GIVE();
    }
    index -= g_projectileCount;
    if (index < g_damageCount)
    {
        Damage* d = &g_damages[index];
        c = (HfContent){size, HF_KIND_DAMAGE, d->id, d->base, 0, d->mod->id, d->mod->folder, d->name, NULL, NULL, 0, NULL};
        return GIVE();
    }
    return 0;
    #undef GIVE
}

/* Vanilla content of a kind by its catalog name (mappings/catalog.json): 1, with its id. */
static int CatalogContent(uint32_t kind, const char* name, uint64_t* id)
{
    for (size_t i = 0; i < sizeof kSdkContent / sizeof kSdkContent[0]; i++)
        if (kSdkContent[i].kind == kind && !strcmp(kSdkContent[i].name, name)) { *id = kSdkContent[i].id; return 1; }
    return 0;
}

/* An entity (or, items, a weapon's loadout id) a mod added under this name, not refused; 0 if none. */
static uint64_t ModEntityNamed(const char* name, int item)
{
    for (int i = 0; i < g_weaponCount; i++)
        if ((!item || !g_weapons[i].plain) && g_weapons[i].state != -1 && !strcmp(g_weapons[i].name, name)) return item ? g_weapons[i].id : g_weapons[i].entity;
    return 0;
}

/* The stratagem type, projectile type or damage id a mod added under this name; 0 if none. */
static uint64_t StratagemNamed(const char* name)
{
    for (uint32_t i = 0; i < g_stratagemCount; i++) if (!strcmp(g_stratagems[i].name, name)) return g_stratagems[i].type;
    return 0;
}

static uint64_t ProjectileTypeNamed(const char* name)
{
    const Projectile* added = ProjectileNamed(name); /* projectiles.c */
    return added != NULL ? added->type : 0;
}

static uint64_t DamageNamed(const char* name)
{
    for (uint32_t i = 0; i < g_damageCount; i++) if (!strcmp(g_damages[i].name, name)) return g_damages[i].id;
    return 0;
}

/* Content by name: vanilla content by its catalog name (mappings/catalog.json), or mod content by its identity
 * ("skyeshade.pelican-support:pelican_gatling", or the full name it was added under). 0 if none. */
static uint64_t ApiFind(uint32_t kind, const char* name)
{
    if (name == NULL || name[0] == 0) return 0;
    uint64_t id = 0;
    if (CatalogContent(kind, name, &id)) return id;
    switch (kind)
    {
    case HF_KIND_ENTITY: return ModEntityNamed(name, 0);
    case HF_KIND_ITEM: return ModEntityNamed(name, 1);
    case HF_KIND_STRATAGEM: return StratagemNamed(name);
    case HF_KIND_PROJECTILE: return ProjectileTypeNamed(name);
    case HF_KIND_DAMAGE: return DamageNamed(name);
    }
    return 0;
}

static void* ApiRecord(HfMod* mod, HfRef entity, const char* component, uint32_t layout, uint32_t size)
{
    const SdkComponent* c = SdkComponentNamed(component);
    if (c == NULL || c->layout != layout || c->stride != size)
    {
        ApiLog(mod, "record: %s %s", component ? component : "(none)", c == NULL ? "has no mappings" :
            "is from another layout; rebuild the mod with this SDK");
        return NULL;
    }
    uint32_t stride = 0;
    void* record = (void*)g_fieldRecord(c->component, entity, &stride, 0);
    if (record == NULL || stride != size)
    {
        ApiLog(mod, "record: %016llx has no %s record of its own%s", (unsigned long long)entity, c->name,
            g_fired[HF_EVENT_ENTITIES_READY] ? "" : " yet (the entity data has not loaded)");
        return NULL;
    }
    return record;
}
