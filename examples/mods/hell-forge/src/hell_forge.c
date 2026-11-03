/* Hell Forge: difficulty 11 (HellForge SDK 5, docs/modding/content.md "A difficulty"; vanilla constants from SDK 6). */
#include "hellforge_mod.h"

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod)
{
    if (api->version < 5 || api->size < offsetof(HfApi, add_difficulty) + sizeof api->add_difficulty) return 1;
    HfDifficulty hell = {sizeof hell};
    hell.name = "skyeshade/hell_forge/hell_forge";
    hell.title = "Hell Forge";
    hell.encounter_points = 1500.0f;   /* Super Helldive: 600 */
    hell.guardforce_points = 1500.0f;  /* Super Helldive: 600 */
    hell.spawner_cooldown = 1.0f;      /* Super Helldive: 1.5 s */
    hell.icon = "skyeshade/hell_forge/icons/hell_forge"; /* images/hell_forge.png, built by hellforge-data.json */
    hell.optional_objectives = 8;      /* Super Helldive: 4 */
    hell.major_objectives = 2;         /* Super Helldive: 1 */
    hell.outpost_scale = 3.0f;         /* three times Super Helldive's enemy outposts (the director handles 128 spawners) */
    hell.mission_minutes = 72.0f;      /* every mission's countdown (Super Helldive: mostly 40 minutes) */
    hell.map_scale = 2.0f;             /* twice Super Helldive's map radius, mega cities included */
    hell.patrol_frequency = 2.0f;      /* roaming patrols twice as often */
    hell.patrol_cap = 1.5f;            /* and up to 1.5 times as many living patrol enemies */
    hell.mission_stratagem = HF_STRATAGEM_M102_GUNNER_FRV; /* every mission grants the M-102 Gunner FRV (no loadout slot) */
    hell.flags = HF_DIFFICULTY_SECOND_PRIMARY | HF_DIFFICULTY_NO_ERADICATE | HF_DIFFICULTY_NO_RAPID_ACQUISITION | HF_DIFFICULTY_RADAR |
        HF_DIFFICULTY_NO_BLITZ | HF_DIFFICULTY_CITY_STRONGHOLDS | HF_DIFFICULTY_CITY_ICBM |
        HF_DIFFICULTY_HEAVY_ARTILLERY;
    /* two missions in one; no Eradicate, Rapid Acquisition or Blitz missions of its own; a radar station marked from the
     * start; fortresses and mega nests in the mega cities too, and every city operation opens with a three-silo ICBM launch;
     * the SEAF Artillery's shells are Explosive, High-Yield Explosive and Mini Nuke only */
    return api->add_difficulty(mod, &hell) == 11 ? 0 : 1;
}
