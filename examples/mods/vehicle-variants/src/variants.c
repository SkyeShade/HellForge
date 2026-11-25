#include "hellforge_mod.h"
#include "hellforge_content.h"

/* A new vehicle is a copy of a vanilla one with records of its own, the way the game makes its own variants: the four
 * exosuits share one body and differ in their arms and a few records. The copy keeps its base's model, so every record
 * naming the model's parts (seats, wheels, damage zones, mounts) still fits it. */

#define M102_GUNNER_FRV 0xCC21C7FFD3EBEFB9ull /* M-102 Gunner FRV (no constant: two entities share its name) */
#define EXO45_RIGHT_GUN 0x08F6089289C83D22ull /* combat_walker_turret, the Patriot's right arm */

/* The Patriot's machine gun arm with a longer belt; a turret's live round count is an 11-bit network field (2047 at most). */
static const HfSet armSet[] = {{HF_FIELD(WeaponMagazine, capacity), HF_U32(2000)}, {0}};
static const HfEntityDef heavyArm = {.size = sizeof(HfEntityDef), .name = "hellforge/vehicle_variants/heavy_patriot_mg",
    .base = EXO45_RIGHT_GUN, .own_all = 1, .set = armSet};
static const HfMountDef heavyMounts[] = {{.slot = 1, .entity = &heavyArm}, {0}};

/* The Patriot turns at 65 degrees a second. */
static const HfSet heavySet[] = {{HF_FIELD(Rotation, rotation_turn_speed), HF_F32(100)}, {0}};
static const HfEntityDef heavyPatriot = {.size = sizeof(HfEntityDef), .name = "hellforge/vehicle_variants/heavy_patriot",
    .base = HF_ENTITY_EXO45_PATRIOT_EXOSUIT, .own_all = 1, .set = heavySet, .mount = heavyMounts,
    .designation = "EXO-45H", .title = "Heavy Patriot Exosuit",
    .description = "A Patriot refitted for long engagements: a quicker turn and a 2000-round machine gun."};

/* The M-102 steers at 3.1 a second (VehicleMotion +0x16C, how fast the wheels follow the stick). */
static const HfSet rallySet[] = {{HF_FIELD(VehicleMotion, steering_response_speed), HF_F32(6.2f)}, {0}};
static const HfEntityDef rallyFrv = {.size = sizeof(HfEntityDef), .name = "hellforge/vehicle_variants/rally_frv",
    .base = M102_GUNNER_FRV, .own_all = 1, .set = rallySet,
    .designation = "M-102R", .title = "Rally FRV", .description = "An M-102 with a quicker steering rack."};

/* A Pelican drop (a base of delivery kind 3) carries payload[0] under payload[1]; the base's vehicle trait keeps it
 * at one vehicle per loadout. */
static uint32_t AddVehicleStratagem(const HfApi* hf, HfMod* mod, uint32_t base, const char* name, const char* code,
    const uint64_t* payload)
{
    HfStratagem s = {sizeof s, .base = base, .name = name, .code = code, .payload = payload, .payload_count = 2};
    uint32_t type = hf->add_stratagem(mod, &s);
    hf->log(mod, "%s: stratagem type %u (0: not added)", name, type);
    return type;
}

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* hf, HfMod* mod)
{
    if (hf->version < 8 || hf->size < offsetof(HfApi, add_stratagem_category) + sizeof hf->add_stratagem_category) return 1;
    static uint64_t patriotPayload[2], frvPayload[2];
    patriotPayload[0] = hf->register_entity(mod, &heavyPatriot); /* registers its arm too */
    frvPayload[0] = hf->register_entity(mod, &rallyFrv);
    if (!patriotPayload[0] || !frvPayload[0]) return 1;
    patriotPayload[1] = frvPayload[1] = HF_ENTITY_PELICAN_TRANSPORT;
    if (!AddVehicleStratagem(hf, mod, HF_STRATAGEM_EXO45_PATRIOT, "hellforge/vehicle_variants/heavy_patriot", "LLDRRUDU", patriotPayload)) return 1;
    if (!AddVehicleStratagem(hf, mod, HF_STRATAGEM_M102_GUNNER_FRV, "hellforge/vehicle_variants/rally_frv", "RRDLLUDU", frvPayload)) return 1;
    return 0;
}
