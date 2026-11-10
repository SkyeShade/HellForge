#include "hellforge_mod.h"
#include "hellforge_content.h"

/* A vehicle with a model of its own on its base's skeleton: the unit is a copy of the M-102's with the mod's mesh
 * (hellforge-data.json, "parts": true), so every joint the M-102's records name (seats, wheels, damage zones, the gun
 * mount) is still there. The body rides the main bone and each wheel the M-102 joint it is named after, so the wheels
 * steer and turn; physics, collision, animations and damage zones stay the M-102's. */

#define M102_GUNNER_FRV 0xCC21C7FFD3EBEFB9ull /* M-102 Gunner FRV (no constant: two entities share its name) */

static const HfEntityDef boxBuggy = {.size = sizeof(HfEntityDef), .name = "hellforge/vehicle_model/box_buggy",
    .base = M102_GUNNER_FRV, .own_all = 1, .unit = "hellforge/vehicle_model/box_buggy",
    .designation = "M-102B", .title = "Box Buggy", .description = "An M-102 under a body of boxes."};

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* hf, HfMod* mod)
{
    if (hf->version < 8 || hf->size < offsetof(HfApi, add_stratagem_category) + sizeof hf->add_stratagem_category) return 1;
    static uint64_t payload[2];
    payload[0] = hf->register_entity(mod, &boxBuggy);
    if (!payload[0]) return 1;
    payload[1] = HF_ENTITY_PELICAN_TRANSPORT; /* a Pelican drop carries payload[0] under payload[1] */
    HfStratagem s = {sizeof s, .base = HF_STRATAGEM_M102_GUNNER_FRV, .name = "hellforge/vehicle_model/box_buggy",
        .code = "RRDLLUDD", .payload = payload, .payload_count = 2};
    uint32_t type = hf->add_stratagem(mod, &s);
    hf->log(mod, "box_buggy: stratagem type %u (0: not added)", type);
    return type ? 0 : 1;
}
