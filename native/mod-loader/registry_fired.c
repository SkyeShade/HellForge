/* What a registered weapon fires, in records of its own (registry_load.c GiveOwnRecords): the beam or arc type its beam or
 * arc record names, and the damage its spray deals. Included by hellforge_modloader.c, after registry.c and before
 * registry_load.c. */

/* The beam or arc a weapon's own record names (+0 of BeamWeaponComponentData, component 270, or ArcWeaponComponentData, 265:
 * the only place a type reaches a weapon). Only an installed type is written: beam_arc.c says why. */
#define REG_BEAM_WEAPON 270u
#define REG_BEAM_WEAPON_STRIDE 120u
#define REG_ARC_WEAPON 265u
#define REG_ARC_WEAPON_STRIDE 80u
#define REG_BEAM_VANILLA 31u
#define REG_ARC_VANILLA 17u

typedef struct FiredKind { const char* what; uint32_t vanilla, component, stride; } FiredKind;
static const FiredKind kFiredBeam = {"beam", REG_BEAM_VANILLA, REG_BEAM_WEAPON, REG_BEAM_WEAPON_STRIDE};
static const FiredKind kFiredArc = {"arc", REG_ARC_VANILLA, REG_ARC_WEAPON, REG_ARC_WEAPON_STRIDE};

static void OwnBeamArc(uint8_t* state, const Weapon* w, uint64_t baseEntity, const FiredKind* k, int usable, uint32_t type)
{
    if (type == 0) return;
    uint32_t c = k->component;
    const char* what = k->what;
    uint8_t* table = StatePointer(state, REG_TABLES + 8 * c);
    uint8_t* record = table != NULL && FindRow(table, SlotsOf(c), 16, baseEntity) != NULL ? OwnRecord(state, c, k->stride, baseEntity, w->entity) : NULL;
    if (record == NULL) { ApiLog(w->mod, "%s: its base has no %s record; it keeps its base's weapon", w->name, what); return; }
    uint32_t base;
    memcpy(&base, record, 4);
    if (base == 0 || base >= k->vanilla) { ApiLog(w->mod, "%s: its own %s record does not name a vanilla %s (%u); it keeps its base's", w->name, what, what, base); return; }
    if (!usable) { ApiLog(w->mod, "%s keeps its base's %s %u: type %u was not added", w->name, what, base, type); return; }
    memcpy(record, &type, 4);
    ApiLog(w->mod, "%s: its %s is type %u (its base's is %u)", w->name, what, type, base);
}

/* The damage its spray deals: +0xC8 of its own SprayWeaponComponentData record (component 315), where the game reads it for
 * every hit of the spray (0x6807B0). A spray has no shared settings row, so the record is the only place (research
 * spray-registry). Only an installed damage is written. */
#define REG_SPRAY_WEAPON 315u
#define REG_SPRAY_WEAPON_STRIDE 224u
#define REG_SPRAY_DAMAGE_AT 0xC8u

static void OwnSpray(uint8_t* state, const Weapon* w, uint64_t baseEntity)
{
    if (w->sprayDamage == 0) return;
    uint8_t* table = StatePointer(state, REG_TABLES + 8 * REG_SPRAY_WEAPON);
    uint8_t* record = table != NULL && FindRow(table, SlotsOf(REG_SPRAY_WEAPON), 16, baseEntity) != NULL
        ? OwnRecord(state, REG_SPRAY_WEAPON, REG_SPRAY_WEAPON_STRIDE, baseEntity, w->entity) : NULL;
    if (record == NULL) { ApiLog(w->mod, "%s: its base does not spray; spray damage %u is not used", w->name, w->sprayDamage); return; }
    uint32_t base;
    memcpy(&base, record + REG_SPRAY_DAMAGE_AT, 4);
    if (!DamageUsable(w->sprayDamage)) { ApiLog(w->mod, "%s keeps its base's spray damage %u: damage %u was not added", w->name, base, w->sprayDamage); return; }
    memcpy(record + REG_SPRAY_DAMAGE_AT, &w->sprayDamage, 4);
    ApiLog(w->mod, "%s: its spray deals damage %u (its base's deals %u)", w->name, w->sprayDamage, base);
}
