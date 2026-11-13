/* Model Carbine: a new arsenal weapon with a model of its own (SDK 2).
 *
 * hellforge-data.json builds the unit skyeshade/model_carbine/carbine from models/carbine.glb: an original carbine of
 * boxes whose joints (hands, sight, attachment points) sit where the AR-23 Liberator has them, drawn with the
 * Liberator's material and given its collision. The weapon handles like the Liberator.
 * Build: scripts/Build-Mod.ps1 examples/mods/model-carbine */
#include "hellforge_mod.h"

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod)
{
    if (api->version < 2) return 1;
    HfWeapon carbine = {sizeof carbine};
    carbine.base = HF_ITEM_AR23_LIBERATOR;
    carbine.unit = "skyeshade/model_carbine/carbine";
    carbine.designation = "MC-1";
    carbine.name = "Model Carbine";
    carbine.description = "A carbine built from a model file of its own. Handles like the Liberator.";
    uint32_t id = api->add_weapon(mod, &carbine);
    api->log(mod, id ? "MC-1 Model Carbine is loadout item %08x" : "the weapon was not added", id);
    return id ? 0 : 1;
}
