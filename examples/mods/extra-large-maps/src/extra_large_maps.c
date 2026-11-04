/* Extra Large Maps (prototype): difficulty 11 at level 10's settings on a ~16 km map (HellForge SDK 5, docs/modding/
 * content.md "A difficulty"; the loader's difficulty_world.c grows the world for a map_scale above 2). */
#include "hellforge_mod.h"
#include <stdlib.h>
#include <windows.h>

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod)
{
    if (api->version < 5 || api->size < offsetof(HfApi, add_difficulty) + sizeof api->add_difficulty) return 1;
    HfDifficulty xl = {sizeof xl};
    xl.name = "skyeshade/extra_large_maps/extra_large";
    xl.title = "Extra Large";
    xl.map_scale = 20.0f;        /* twenty times Super Helldive's map radius: a border ~7,960 m out, a world of 16,384 m */
    char scale[16] = ""; /* research: HELLFORGE_XL_SCALE=<map_scale> (3..20) for the smaller sizes on the way up */
    if (GetEnvironmentVariableA("HELLFORGE_XL_SCALE", scale, sizeof scale) > 0 && atof(scale) >= 3.0 && atof(scale) <= 20.0) xl.map_scale = (float)atof(scale);
    xl.mission_minutes = 75.0f;  /* the longest countdown a level can have: crossing the map on foot takes most of it */
    xl.mission_stratagem = HF_STRATAGEM_M102_GUNNER_FRV; /* every mission grants the M-102 Gunner FRV (no loadout slot) */
    /* Difficulty 11's content and pressure (Hell Forge's values): the map is an operation's missions in one, so it starts
     * from a level-11 mission and grows from there. */
    xl.encounter_points = 1500.0f;   /* Super Helldive: 600 */
    xl.guardforce_points = 1500.0f;  /* Super Helldive: 600 */
    xl.spawner_cooldown = 1.0f;      /* Super Helldive: 1.5 s */
    xl.optional_objectives = 8;      /* Super Helldive: 4 */
    xl.major_objectives = 2;         /* Super Helldive: 1 */
    xl.outpost_scale = 3.0f;         /* three times Super Helldive's enemy outposts */
    xl.patrol_frequency = 2.0f;
    xl.patrol_cap = 1.5f;
    xl.flags = HF_DIFFICULTY_SECOND_PRIMARY | HF_DIFFICULTY_NO_ERADICATE | HF_DIFFICULTY_NO_RAPID_ACQUISITION | HF_DIFFICULTY_RADAR |
        HF_DIFFICULTY_NO_BLITZ | HF_DIFFICULTY_CITY_STRONGHOLDS | HF_DIFFICULTY_CITY_ICBM;
    return api->add_difficulty(mod, &xl) == 11 ? 0 : 1;
}
