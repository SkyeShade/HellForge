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
    return api->add_difficulty(mod, &xl) == 11 ? 0 : 1;
}
