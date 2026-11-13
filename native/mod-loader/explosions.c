/* New explosion types (SDK 17, docs/modding/content.md "Projectiles and damage"): copies of vanilla ExplosionSettings rows
 * under types of their own, so a projectile can blow up with its own damage and radii. Included by hellforge_modloader.c,
 * after damage.c and before projectiles.c. The list's move, its bounds and network field 248 are in docs/internals.md
 * "Explosions, beams and arcs".
 *
 * Every bound rises: one left behind gives the default row, not a crash. 0x1389C9A compares with 423 as well but walks the
 * name-hash table at 0x21D3730 and stays: a new type has no name. */

#define EXP_TABLE 0x37CC920u
#define EXP_VANILLA 423u
#define EXP_NARROW_MAX 89u /* types 423 .. 511, what field 248 carries at the game's 9 bits */
#define EXP_MAX_NEW 512u   /* with the field at 10 bits */
#define EXP_FIELD 248u
#define EXP_FIELD_BITS 9u
#define EXP_FIELD_WIDE 10u
#define EXP_ROW 152u
#define EXP_DAMAGE_AT 4u
#define EXP_INNER_AT 0x10u
#define EXP_OUTER_AT 0x14u
#define EXP_SHOCKWAVE_AT 0x18u
#define EXP_ARC_AT 0x78u
#define EXP_CLEAR 0x11DC39Eu /* 41 B8 38 0D 00 00: mov r8d, 0xD38 (423 slots) */

_Static_assert(EXP_VANILLA + EXP_NARROW_MAX == 1u << EXP_FIELD_BITS, "the types the game's width carries");
_Static_assert(EXP_VANILLA + EXP_MAX_NEW <= 1u << EXP_FIELD_WIDE, "the types the wide field carries");

static const uint8_t kExplosionClearPin[6] = {0x41, 0xb8, 0x38, 0x0d, 0x00, 0x00};

#include "explosion_sites.h"

typedef struct Explosion
{
    HfMod* mod;
    char name[128];
    uint32_t type, base, damage, arc;
    float inner, outer, shockwave;
    uint8_t* row;
} Explosion;

static Explosion g_explosions[EXP_MAX_NEW];
static uint32_t g_explosionCount;
static uint32_t g_explosionsIn; /* of g_explosionCount, what installed */
static int g_explosionState; /* 0 waiting, 1 installed, -1 refused */
static uint8_t* g_explosionTable;
static int (*g_explosionFields)(uintptr_t* rows, uint32_t* count) = FieldTypes; /* the native fixture replaces it */

/* Whether the definition cannot go in; the mod's log says why. arc: the one it names (0 from an older HfExplosion). */
static int ExplosionRefused(HfMod* mod, const HfExplosion* def, const char* name, uint32_t arc)
{
    if (g_explosionState != 0 || g_fired[HF_EVENT_PROJECTILES_READY]) { ApiLog(mod, "add_explosion(%s): too late; call it from hellforge_mod_init", name); return 1; }
    if (g_explosionCount >= EXP_MAX_NEW) { ApiLog(mod, "add_explosion(%s): mods added %u explosion types already", name, EXP_MAX_NEW); return 1; }
    if (def->base == 0 || def->base >= EXP_VANILLA) { ApiLog(mod, "add_explosion(%s): base %u is not a vanilla explosion type (1 .. 422)", name, def->base); return 1; }
    if (def->damage != 0 && def->damage >= DMG_VANILLA + g_damageCount) { ApiLog(mod, "add_explosion(%s): damage %u is neither a vanilla damage id nor one a mod added", name, def->damage); return 1; }
    if (!(def->inner_radius >= 0.0f && def->outer_radius >= 0.0f && def->shockwave_radius >= 0.0f)) { ApiLog(mod, "add_explosion(%s): a radius is negative", name); return 1; }
    if (!BeamArcKnown(&g_arcs, arc)) { ApiLog(mod, "add_explosion(%s): arc %u is neither a vanilla type nor one a mod added", name, arc); return 1; }
    for (uint32_t i = 0; i < g_explosionCount; i++)
        if (strcmp(g_explosions[i].name, name) == 0) { ApiLog(mod, "add_explosion(%s): already added by %s", name, g_explosions[i].mod->id); return 1; }
    return 0;
}

static uint32_t ApiAddExplosion(HfMod* mod, const HfExplosion* def)
{
    if (mod == NULL || def == NULL || def->size < offsetof(HfExplosion, shockwave_radius) + sizeof def->shockwave_radius)
    {
        ApiLog(mod, "add_explosion: no explosion, or its size is not set (HfExplosion e = {sizeof e})");
        return 0;
    }
    char identity[256];
    const char* name = def->name != NULL && def->name[0] ? Identity(mod, def->name, identity, sizeof identity) : "(unnamed)";
    uint32_t arc = def->size >= offsetof(HfExplosion, arc) + sizeof def->arc ? def->arc : 0;
    if (ExplosionRefused(mod, def, name, arc)) return 0;
    Explosion* e = &g_explosions[g_explosionCount];
    memset(e, 0, sizeof *e);
    e->mod = mod;
    snprintf(e->name, sizeof e->name, "%s", name);
    e->type = EXP_VANILLA + g_explosionCount;
    e->base = def->base;
    e->damage = def->damage;
    e->arc = arc;
    e->inner = def->inner_radius;
    e->outer = def->outer_radius;
    e->shockwave = def->shockwave_radius;
    g_explosionCount++;
    ApiLog(mod, "adding explosion %s as type %u: a copy of type %u%s", e->name, e->type, e->base, e->damage ? ", its own damage" : "");
    return e->type;
}

static uintptr_t ApiExplosionRow(uint32_t type)
{
    if (g_explosionState != 1 || type < EXP_VANILLA || type >= EXP_VANILLA + g_explosionsIn) return 0;
    return (uintptr_t)g_explosions[type - EXP_VANILLA].row;
}

static uint64_t ExplosionNamed(const char* name)
{
    for (uint32_t i = 0; i < g_explosionCount; i++) if (!strcmp(g_explosions[i].name, name)) return g_explosions[i].type;
    return 0;
}

/* Whether an explosion type resolves in this game: vanilla, or added and installed. */
static int ExplosionUsable(uint32_t type)
{
    if (type < EXP_VANILLA) return 1;
    return g_explosionState == 1 && type < EXP_VANILLA + g_explosionsIn;
}

/* The vanilla type a type copies (itself when it is vanilla): what its effect's packages are. */
static uint32_t ExplosionBaseOf(uint32_t type)
{
    return type >= EXP_VANILLA && type < EXP_VANILLA + g_explosionCount ? g_explosions[type - EXP_VANILLA].base : type;
}

static const char* ExplosionSitesFailed(uintptr_t game)
{
    uint8_t clear[sizeof kExplosionClearPin];
    if (SafeRead(game + EXP_CLEAR, clear, sizeof clear) || memcmp(clear, kExplosionClearPin, sizeof clear) != 0)
        return "the explosion list's loader does not clear the reviewed 423 slots";
    for (size_t i = 0; i < sizeof kExplosionSites / sizeof kExplosionSites[0]; i++)
    {
        int32_t disp;
        if (SafeRead(game + kExplosionSites[i].rva + kExplosionSites[i].dispAt, &disp, 4) ||
            disp != SiteDisplacement(game + EXP_TABLE, game, &kExplosionSites[i]))
            return "an instruction addressing the explosion list is not the reviewed one";
    }
    for (size_t i = 0; i < sizeof kExplosionBounds / sizeof kExplosionBounds[0]; i++)
    {
        uint32_t bound;
        if (SafeRead(game + kExplosionBounds[i].rva + kExplosionBounds[i].immAt, &bound, 4) || bound != EXP_VANILLA)
            return "a bound of the explosion list is not the reviewed one";
    }
    return NULL;
}

static int RefuseExplosions(const char* why)
{
    g_explosionState = -1;
    Note("explosions: %u new type(s) refused: %s; projectiles keep their own explosions", g_explosionCount, why);
    for (uint32_t i = 0; i < g_explosionCount; i++) ApiLog(g_explosions[i].mod, "explosion %s (type %u) not added: %s", g_explosions[i].name, g_explosions[i].type, why);
    return 1;
}

/* A new type's row (row: where it goes): its base's, its own type, damage and radii. NULL, or why not. */
static const char* ExplosionRow(Explosion* e, uint8_t* row, uintptr_t game)
{
    uint64_t base = 0;
    if (SafeRead(game + EXP_TABLE + (uintptr_t)e->base * 8, &base, 8) || base == 0 || SafeRead(base, row, EXP_ROW) || *(uint32_t*)row != e->base)
        return "a base explosion row does not name its type";
    memcpy(row, &e->type, 4);
    if (e->damage != 0 && DamageUsable(e->damage)) memcpy(row + EXP_DAMAGE_AT, &e->damage, 4);
    else if (e->damage != 0) ApiLog(e->mod, "explosion %s keeps its base's damage: damage %u was not added", e->name, e->damage);
    if (e->arc != 0 && BeamArcUsable(&g_arcs, e->arc)) memcpy(row + EXP_ARC_AT, &e->arc, 4);
    else if (e->arc != 0) ApiLog(e->mod, "explosion %s keeps its base's arc: arc %u was not added", e->name, e->arc);
    if (e->inner != 0.0f) memcpy(row + EXP_INNER_AT, &e->inner, 4);
    if (e->outer != 0.0f) memcpy(row + EXP_OUTER_AT, &e->outer, 4);
    if (e->shockwave != 0.0f) memcpy(row + EXP_SHOCKWAVE_AT, &e->shockwave, 4);
    e->row = row;
    return NULL;
}

/* The instructions that address the list, pointed at HellForge's. NULL, or why not. */
static const char* ExplosionMove(uintptr_t game, const uint8_t* table, PutFn put)
{
    for (size_t i = 0; i < sizeof kExplosionSites / sizeof kExplosionSites[0]; i++)
    {
        int32_t disp = SiteDisplacement((uintptr_t)table, game, &kExplosionSites[i]);
        if (put(game + kExplosionSites[i].rva + kExplosionSites[i].dispAt, &disp, 4))
        {
            /* Unreachable in practice (the pages are writable); the list stays usable through both copies. */
            Note("explosions: could not rewrite the instruction at 0x%x; %zu of %zu moved", kExplosionSites[i].rva, i, sizeof kExplosionSites / sizeof kExplosionSites[0]);
            return "an instruction could not be rewritten";
        }
    }
    return NULL;
}

/* Every bound raised to `bound`, its high byte first: one passing 0x1FF must not read lower half way. NULL, or why not. */
static const char* ExplosionRaise(uintptr_t game, uint32_t bound, PutFn put)
{
    const uint8_t* bytes = (const uint8_t*)&bound;
    for (size_t i = 0; i < sizeof kExplosionBounds / sizeof kExplosionBounds[0]; i++)
    {
        uintptr_t at = game + kExplosionBounds[i].rva + kExplosionBounds[i].immAt;
        if (put(at + 1, bytes + 1, 1) || put(at, bytes, 1)) return "could not raise the type bounds";
    }
    return NULL;
}

/* Field 248's row when it is as reviewed (an integer of the game's 9 bits from 0), or 0. */
static uintptr_t ExplosionFieldRow(uintptr_t rows, uint32_t count)
{
    uint8_t row[0x18];
    uint32_t min;
    if (EXP_FIELD >= count || SafeRead(rows + (uintptr_t)EXP_FIELD * 0x18, row, sizeof row)) return 0;
    memcpy(&min, row + 16, 4);
    return row[12] == 1 && row[13] == EXP_FIELD_BITS && min == 0 ? rows + (uintptr_t)EXP_FIELD * 0x18 : 0;
}

/* One line for each new type. Its pointers: the row's aligned words that read as addresses. A copy outlives the retained
 * resource's reload, so any of them is worth a look. */
static void ExplosionsInstalledLog(void)
{
    for (uint32_t i = g_explosionsIn; i < g_explosionCount; i++)
        ApiLog(g_explosions[i].mod, "explosion %s (type %u) not added: the type does not fit the game's 9-bit field", g_explosions[i].name, g_explosions[i].type);
    for (uint32_t i = 0; i < g_explosionsIn; i++)
    {
        const Explosion* e = &g_explosions[i];
        uint32_t damage, pointers = 0;
        float inner, outer;
        if (e->row == NULL) continue;
        memcpy(&damage, e->row + EXP_DAMAGE_AT, 4);
        memcpy(&inner, e->row + EXP_INNER_AT, 4);
        memcpy(&outer, e->row + EXP_OUTER_AT, 4);
        for (uint32_t at = 0; at + 8 <= EXP_ROW; at += 8)
        {
            uint64_t word;
            memcpy(&word, e->row + at, 8);
            pointers += word >= 0x10000000000ull && word < 0x800000000000ull;
        }
        ApiLog(e->mod, "explosion %s is type %u: damage id %u, radii %g / %g; %u word(s) of its row read as addresses", e->name, e->type, damage, (double)inner, (double)outer, pointers);
    }
}

/* Field 248's row in *field when a set past EXP_NARROW_MAX can widen it, else 0. 0 while the field types are not there yet. */
static int ExplosionField(uintptr_t* field, int pinsReady)
{
    *field = 0;
    if (g_explosionCount <= EXP_NARROW_MAX) return 1;
    uintptr_t rows;
    uint32_t count;
    if (g_explosionFields(&rows, &count)) { if (!pinsReady) return 0; }
    else *field = ExplosionFieldRow(rows, count);
    if (*field == 0) Note("explosions: network field type %u is not the reviewed one: the first %u of %u new types go in", EXP_FIELD, EXP_NARROW_MAX, g_explosionCount);
    return 1;
}

/* The moved list, whole: the vanilla pointers, the new rows and theirs. NULL, and why not. */
static uint8_t* ExplosionList(uintptr_t game, const char** why)
{
    size_t listBytes = (size_t)(EXP_VANILLA + EXP_MAX_NEW) * 8;
    uint8_t* table = AllocateNearGame(game, listBytes + (size_t)g_explosionsIn * EXP_ROW);
    *why = "no memory within 2 GB of game.dll for the moved list";
    if (table == NULL) return NULL;
    *why = "the explosion list is unreadable";
    if (SafeRead(game + EXP_TABLE, table, EXP_VANILLA * 8)) return NULL;
    for (uint32_t i = 0; i < g_explosionsIn; i++)
        if ((*why = ExplosionRow(&g_explosions[i], table + listBytes + (size_t)i * EXP_ROW, game)) != NULL) return NULL;
    for (uint32_t i = 0; i < g_explosionsIn; i++) memcpy(table + (size_t)g_explosions[i].type * 8, &g_explosions[i].row, 8);
    return table;
}

/* The game onto the complete list: the instructions, then the bounds, so no code reads a new type before its instructions
 * read the new list; then the field. NULL, or why not. */
static const char* ExplosionSwitch(uintptr_t game, uint8_t* table, uintptr_t field)
{
    const char* why = ExplosionMove(game, table, ApiWrite);
    if (why != NULL) return why;
    g_explosionTable = table;
    if ((why = ExplosionRaise(game, EXP_VANILLA + g_explosionsIn, ApiWrite)) != NULL) return why;
    uint8_t wide = EXP_FIELD_WIDE;
    return field != 0 && ApiWrite(field + 13, &wide, 1) ? "could not widen the network field" : NULL;
}

/* Polled with the projectile list (InstallProjectiles) until it returns 1: moves the list, adds the rows and raises the
 * bounds once the game has loaded its explosion settings, and for a set past EXP_NARROW_MAX its network configuration.
 * pinsReady: the caller's grace period for code still being decoded is over. */
static int InstallExplosions(uintptr_t game, int pinsReady)
{
    if (g_explosionState != 0) return 1;
    if (g_explosionCount == 0) { g_explosionState = 1; return 1; }
    uint64_t first = 0;
    if (SafeRead(game + EXP_TABLE + 8, &first, 8) || first == 0) return 0; /* not loaded yet */
    const char* why = ExplosionSitesFailed(game);
    if (why != NULL) return pinsReady ? RefuseExplosions(why) : 0;
    uintptr_t field;
    if (!ExplosionField(&field, pinsReady)) return 0;
    g_explosionsIn = field != 0 || g_explosionCount <= EXP_NARROW_MAX ? g_explosionCount : EXP_NARROW_MAX;
    uint8_t* table = ExplosionList(game, &why);
    if (table == NULL) return RefuseExplosions(why);
    if ((why = ExplosionSwitch(game, table, field)) != NULL) return RefuseExplosions(why);
    g_explosionState = 1;
    Note("explosions: the list moved to 0x%llx (%zu instructions, %zu bounds raised to %u) with %u new type(s)%s", (unsigned long long)(uintptr_t)table,
        sizeof kExplosionSites / sizeof kExplosionSites[0], sizeof kExplosionBounds / sizeof kExplosionBounds[0], EXP_VANILLA + g_explosionsIn, g_explosionsIn,
        field != 0 ? ", network field type 248 now 10 bits" : "");
    ExplosionsInstalledLog();
    return 1;
}

/* The index-th installed explosion, beam or arc a mod added, into *c; 0 past the last, with index taken off for the next
 * kind. */
static int AddedRowContent(uint32_t* index, uint32_t size, HfContent* c)
{
    if (g_explosionState == 1 && *index < g_explosionsIn)
    {
        const Explosion* e = &g_explosions[*index];
        *c = (HfContent){size, HF_KIND_EXPLOSION, e->type, e->base, e->damage, e->mod->id, e->mod->folder, e->name, NULL, NULL, 0, NULL};
        return 1;
    }
    if (g_explosionState == 1) *index -= g_explosionsIn;
    const RowKind* kinds[] = {&g_beams, &g_arcs};
    for (int k = 0; k < 2; k++)
    {
        if (kinds[k]->state != 1) continue;
        if (*index < kinds[k]->count)
        {
            const AddedRow* a = &kinds[k]->added[*index];
            *c = (HfContent){size, k ? HF_KIND_ARC : HF_KIND_WEAPON_BEAM, a->type, a->base, a->damage, a->mod->id, a->mod->folder, a->name, NULL, NULL, 0, NULL};
            return 1;
        }
        *index -= kinds[k]->count;
    }
    return 0;
}
