/* New beam and arc types (SDK 17, docs/modding/content.md "Projectiles and damage"): copies of vanilla BeamSettings and
 * ArcSettings rows under types of their own. A weapon's own record names only the type: damage, radius, length, range and
 * chains are all in the shared row, so a beam or arc weapon with stats of its own needs a type of its own. Included by
 * hellforge_modloader.c, after damage.c and before explosions.c. The lists' moves are in docs/internals.md "Explosions,
 * beams and arcs".
 *
 * No code bounds a type: a reader takes the default row for 0 and anything else as an index. So an unfilled slot is a null
 * row and a crash, not a fallback, and only a type this says is usable (BeamArcUsable) may reach a record or a row. */

#define BA_MAX_NEW 512u
#define ARC_TABLE 0x37C5F90u
#define ARC_VANILLA 17u
#define ARC_ROW 104u
#define ARC_DAMAGE_AT 0x24u
#define BEAM_TABLE 0x37C8250u
#define BEAM_VANILLA 31u
#define BEAM_ROW 112u
#define BEAM_DAMAGE_AT 0x0Cu
#define BEAM_RADIUS_AT 0x04u
#define BEAM_LENGTH_AT 0x08u

#include "beam_arc_sites.h"

typedef struct AddedRow
{
    HfMod* mod;
    char name[128];
    uint32_t type, base, damage;
    float radius, length; /* a beam's; 0 keeps the base's */
    uint8_t* row;
} AddedRow;

typedef struct RowKind
{
    const char* what; /* "beam", "arc" */
    uint32_t table, vanilla, stride, damageAt;
    const DamageSite* sites;
    size_t siteCount;
    AddedRow* added;
    uint32_t count;
    int state; /* 0 waiting, 1 installed, -1 refused */
    uint8_t* list;
} RowKind;

static AddedRow g_addedBeams[BA_MAX_NEW], g_addedArcs[BA_MAX_NEW];
static RowKind g_beams = {"beam", BEAM_TABLE, BEAM_VANILLA, BEAM_ROW, BEAM_DAMAGE_AT, kBeamSites, sizeof kBeamSites / sizeof kBeamSites[0], g_addedBeams};
static RowKind g_arcs = {"arc", ARC_TABLE, ARC_VANILLA, ARC_ROW, ARC_DAMAGE_AT, kArcSites, sizeof kArcSites / sizeof kArcSites[0], g_addedArcs};

/* A new type's entry, or 0 and the log says why. */
static uint32_t AddRow(RowKind* k, HfMod* mod, const char* given, uint32_t base, uint32_t damage, float radius, float length)
{
    char identity[256];
    const char* name = given != NULL && given[0] ? Identity(mod, given, identity, sizeof identity) : "(unnamed)";
    if (k->state != 0 || g_fired[HF_EVENT_PROJECTILES_READY]) { ApiLog(mod, "add_%s(%s): too late; call it from hellforge_mod_init", k->what, name); return 0; }
    if (k->count >= BA_MAX_NEW) { ApiLog(mod, "add_%s(%s): mods added %u %s types already", k->what, name, BA_MAX_NEW, k->what); return 0; }
    if (base == 0 || base >= k->vanilla) { ApiLog(mod, "add_%s(%s): base %u is not a vanilla %s type (1 .. %u)", k->what, name, base, k->what, k->vanilla - 1); return 0; }
    if (damage != 0 && damage >= DMG_VANILLA + g_damageCount) { ApiLog(mod, "add_%s(%s): damage %u is neither a vanilla damage id nor one a mod added", k->what, name, damage); return 0; }
    if (!(radius >= 0.0f && length >= 0.0f)) { ApiLog(mod, "add_%s(%s): a size is negative", k->what, name); return 0; }
    for (uint32_t i = 0; i < k->count; i++)
        if (strcmp(k->added[i].name, name) == 0) { ApiLog(mod, "add_%s(%s): already added by %s", k->what, name, k->added[i].mod->id); return 0; }
    AddedRow* a = &k->added[k->count];
    memset(a, 0, sizeof *a);
    a->mod = mod;
    snprintf(a->name, sizeof a->name, "%s", name);
    a->type = k->vanilla + k->count;
    a->base = base;
    a->damage = damage;
    a->radius = radius;
    a->length = length;
    k->count++;
    ApiLog(mod, "adding %s %s as type %u: a copy of type %u%s", k->what, a->name, a->type, base, damage ? ", its own damage" : "");
    return a->type;
}

static uint32_t ApiAddBeam(HfMod* mod, const HfBeam* def)
{
    if (mod == NULL || def == NULL || def->size < offsetof(HfBeam, length) + sizeof def->length)
    {
        ApiLog(mod, "add_beam: no beam, or its size is not set (HfBeam b = {sizeof b})");
        return 0;
    }
    return AddRow(&g_beams, mod, def->name, def->base, def->damage, def->radius, def->length);
}

static uint32_t ApiAddArc(HfMod* mod, const HfArc* def)
{
    if (mod == NULL || def == NULL || def->size < offsetof(HfArc, damage) + sizeof def->damage)
    {
        ApiLog(mod, "add_arc: no arc, or its size is not set (HfArc a = {sizeof a})");
        return 0;
    }
    return AddRow(&g_arcs, mod, def->name, def->base, def->damage, 0.0f, 0.0f);
}

static uintptr_t RowOfType(const RowKind* k, uint32_t type)
{
    if (k->state != 1 || type < k->vanilla || type >= k->vanilla + k->count) return 0;
    return (uintptr_t)k->added[type - k->vanilla].row;
}
static uintptr_t ApiBeamRow(uint32_t type) { return RowOfType(&g_beams, type); }
static uint64_t RowNamed(const RowKind* k, const char* name)
{
    for (uint32_t i = 0; i < k->count; i++) if (!strcmp(k->added[i].name, name)) return k->added[i].type;
    return 0;
}
static uintptr_t ApiArcRow(uint32_t type) { return RowOfType(&g_arcs, type); }

/* Whether a type's slot holds a row now: vanilla, or added and installed. Nothing else may reach a record or a row. */
static int BeamArcUsable(const RowKind* k, uint32_t type)
{
    if (type < k->vanilla) return 1;
    return k->state == 1 && type < k->vanilla + k->count;
}

/* Whether a mod may name the type at all (it may not be installed yet). */
static int BeamArcKnown(const RowKind* k, uint32_t type) { return type < k->vanilla + k->count; }

_Static_assert(REG_BEAM_VANILLA == BEAM_VANILLA && REG_ARC_VANILLA == ARC_VANILLA, "the weapon registry's check of a record");
static int BeamTypeUsable(uint32_t type) { return BeamArcUsable(&g_beams, type); }
static int ArcTypeUsable(uint32_t type) { return BeamArcUsable(&g_arcs, type); }

static int RefuseRows(RowKind* k, const char* why)
{
    k->state = -1;
    Note("%ss: %u new type(s) refused: %s; weapons keep their base's", k->what, k->count, why);
    for (uint32_t i = 0; i < k->count; i++) ApiLog(k->added[i].mod, "%s %s (type %u) not added: %s", k->what, k->added[i].name, k->added[i].type, why);
    return 1;
}

/* A new type's row (row: where it goes): its base's, its own type and damage. NULL, or why not. */
static const char* AddedRowMake(const RowKind* k, AddedRow* a, uint8_t* row, uintptr_t game)
{
    uint64_t base = 0;
    if (SafeRead(game + k->table + (uintptr_t)a->base * 8, &base, 8) || base == 0 || SafeRead(base, row, k->stride) || *(uint32_t*)row != a->base)
        return "a base row does not name its type";
    memcpy(row, &a->type, 4);
    if (a->damage != 0 && DamageUsable(a->damage)) memcpy(row + k->damageAt, &a->damage, 4);
    else if (a->damage != 0) ApiLog(a->mod, "%s %s keeps its base's damage: damage %u was not added", k->what, a->name, a->damage);
    if (a->radius != 0.0f) memcpy(row + BEAM_RADIUS_AT, &a->radius, 4);
    if (a->length != 0.0f) memcpy(row + BEAM_LENGTH_AT, &a->length, 4);
    a->row = row;
    return NULL;
}

/* One line for each new type, with the row's aligned words that read as addresses (explosions.c says why they matter). */
static void RowsInstalledLog(const RowKind* k)
{
    for (uint32_t i = 0; i < k->count; i++)
    {
        const AddedRow* a = &k->added[i];
        uint32_t damage, pointers = 0;
        memcpy(&damage, a->row + k->damageAt, 4);
        for (uint32_t at = 0; at + 8 <= k->stride; at += 8)
        {
            uint64_t word;
            memcpy(&word, a->row + at, 8);
            pointers += word >= 0x10000000000ull && word < 0x800000000000ull;
        }
        ApiLog(a->mod, "%s %s is type %u: damage id %u; %u word(s) of its row read as addresses", k->what, a->name, a->type, damage, pointers);
    }
}

/* Whether every instruction addressing the list is the reviewed one. */
static int RowSitesReviewed(const RowKind* k, uintptr_t game)
{
    for (size_t i = 0; i < k->siteCount; i++)
    {
        int32_t disp;
        if (SafeRead(game + k->sites[i].rva + k->sites[i].dispAt, &disp, 4) || disp != SiteDisplacement(game + k->table, game, &k->sites[i])) return 0;
    }
    return 1;
}

/* The moved list, whole: the vanilla pointers, the new rows and theirs. NULL, and why not. */
static uint8_t* RowListMake(RowKind* k, uintptr_t game, const char** why)
{
    size_t listBytes = (size_t)(k->vanilla + BA_MAX_NEW) * 8;
    uint8_t* list = AllocateNearGame(game, listBytes + (size_t)k->count * k->stride);
    *why = "no memory within 2 GB of game.dll for the moved list";
    if (list == NULL) return NULL;
    *why = "the list is unreadable";
    if (SafeRead(game + k->table, list, (size_t)k->vanilla * 8)) return NULL;
    for (uint32_t i = 0; i < k->count; i++)
        if ((*why = AddedRowMake(k, &k->added[i], list + listBytes + (size_t)i * k->stride, game)) != NULL) return NULL;
    for (uint32_t i = 0; i < k->count; i++) memcpy(list + (size_t)k->added[i].type * 8, &k->added[i].row, 8);
    return list;
}

/* The instructions that address the list, pointed at the complete copy. NULL, or why not. */
static const char* RowListSwitch(const RowKind* k, uintptr_t game, const uint8_t* list)
{
    for (size_t i = 0; i < k->siteCount; i++)
    {
        int32_t disp = SiteDisplacement((uintptr_t)list, game, &k->sites[i]);
        if (ApiWrite(game + k->sites[i].rva + k->sites[i].dispAt, &disp, 4))
        {
            /* Unreachable in practice (the pages are writable); the list stays usable through both copies. */
            Note("%ss: could not rewrite the instruction at 0x%x; %zu of %zu moved", k->what, k->sites[i].rva, i, k->siteCount);
            return "an instruction could not be rewritten";
        }
    }
    return NULL;
}

/* Polled with the projectile list until it returns 1, as InstallExplosions is. */
static int InstallRows(RowKind* k, uintptr_t game, int pinsReady)
{
    if (k->state != 0) return 1;
    if (k->count == 0) { k->state = 1; return 1; }
    uint64_t first = 0;
    if (SafeRead(game + k->table + 8, &first, 8) || first == 0) return 0; /* not loaded yet */
    if (!RowSitesReviewed(k, game)) return pinsReady ? RefuseRows(k, "an instruction addressing the list is not the reviewed one") : 0;
    const char* why;
    uint8_t* list = RowListMake(k, game, &why);
    if (list == NULL) return RefuseRows(k, why);
    if ((why = RowListSwitch(k, game, list)) != NULL) return RefuseRows(k, why);
    k->list = list;
    k->state = 1;
    Note("%ss: the list moved to 0x%llx (%zu instructions) with %u new type(s)", k->what, (unsigned long long)(uintptr_t)list, k->siteCount, k->count);
    RowsInstalledLog(k);
    return 1;
}
