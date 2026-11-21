/* Projectile cap probe (in-game test only, never shipped; tests/in-game/projectile-cap.json): room for 1,024 new
 * projectile types. Adds 400 copies of the AR-23 Liberator's round (types 351 to 750), past the 161 the game's 9-bit
 * co-op field carries, so HellForge widens that field to 11 bits; once the list is in, checks that the first, the 161st,
 * the 162nd and the last have their rows, each naming its own type. "PROBE <check> ok" or "PROBE <check> FAIL <why>". */
#include <stdio.h>
#include "hellforge_mod.h"
#include "hellforge_content.h"

#define COUNT 400
static HfMod* g_mod;
static const HfApi* hf;
static uint32_t g_types[COUNT];

static void Ready(HfMod* mod, HfEvent event, void* user)
{
    (void)mod; (void)event; (void)user;
    static const int probes[] = {0, 160, 161, COUNT - 1};
    int good = 0;
    for (int k = 0; k < 4; k++)
    {
        uintptr_t row = g_types[probes[k]] ? hf->projectile_row(g_types[probes[k]]) : 0;
        uint32_t type = 0;
        if (row && hf->read(row, &type, 4) == 0 && type == g_types[probes[k]]) good++;
        else hf->log(g_mod, "PROBE row FAIL type %u has no row naming it (row %p, reads %u)", g_types[probes[k]], (void*)row, type);
    }
    if (good == 4) hf->log(g_mod, "PROBE rows ok: types %u, %u, %u and %u each have their row", g_types[0], g_types[160], g_types[161], g_types[COUNT - 1]);
}

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod)
{
    hf = api;
    g_mod = mod;
    if (api->version < 3) { api->log(mod, "PROBE sdk FAIL the loader is SDK %u", api->version); return 1; }
    api->log(mod, "PROBE sdk ok");
    int added = 0;
    for (int i = 0; i < COUNT; i++)
    {
        char name[64];
        snprintf(name, sizeof name, "round_%03d", i);
        HfProjectile p = {.size = sizeof p, .base = HF_PROJECTILE_AR23_LIBERATOR, .name = name};
        g_types[i] = api->add_projectile(mod, &p);
        added += g_types[i] != 0;
    }
    if (added == COUNT && g_types[COUNT - 1] == 351 + COUNT - 1) api->log(mod, "PROBE added ok: %d types, %u to %u", added, g_types[0], g_types[COUNT - 1]);
    else api->log(mod, "PROBE added FAIL %d of %d (last type %u)", added, COUNT, g_types[COUNT - 1]);
    api->on(mod, HF_EVENT_PROJECTILES_READY, Ready, NULL);
    return 0;
}
