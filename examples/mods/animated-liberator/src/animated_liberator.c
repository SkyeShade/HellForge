/* AR-23 Liberator Animated: a new arsenal weapon with animations of its own (SDK 2).
 *
 * It is a copy of the AR-23 Liberator with its own loadout id, name and unit. hellforge-data.json copies the
 * Liberator's unit and gives it the bones, clips and state machine HellForge builds from animations/assault_rifle.json:
 * the bolt kicks back on every shot and the magazine drops out and comes back on a reload.
 * Build: scripts/Build-Mod.ps1 examples/mods/animated-liberator */
#include "hellforge_mod.h"

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod)
{
    if (api->version < 2) return 1;
    HfWeapon rifle = {sizeof rifle};
    rifle.base = HF_ITEM_AR23_LIBERATOR;
    rifle.unit = "skyeshade/animated_liberator/assault_rifle";
    rifle.designation = "AR-23A";
    rifle.name = "Liberator Animated";
    rifle.description = "The SEAF standard assault rifle with a bolt you can see working and a magazine change you can follow.";
    uint32_t id = api->add_weapon(mod, &rifle);
    api->log(mod, id ? "AR-23A Liberator Animated is loadout item %08x" : "the weapon was not added", id);
    return id ? 0 : 1;
}
