/* Stratagem codes probe (in-game test only, never shipped; tests/in-game/stratagem-codes.json). Its stats give Resupply
 * (type 33) the input code Up Up Left Right Down (packed 2231: native/mod-loader/stratagem_codes.c). It logs every
 * beacon's type, so the harness's call of the new code can be checked (a vanilla row is not reachable through the SDK:
 * stratagem_row is for added stratagems; the loader's stats line shows the packed code written). */
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <windows.h>
#include "hellforge_mod.h"
#include "hellforge_content.h"

static HfMod* g_mod;
static const HfApi* hf;
#define LOG(...) hf->log(g_mod, __VA_ARGS__)

static void Called(HfMod* mod, const HfStratagemEvent* event, void* user)
{
    (void)mod; (void)user;
    LOG("PROBE beacon of type %u", event->type);
}

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod)
{
    hf = api;
    g_mod = mod;
    if (api->version < 16) { api->log(mod, "PROBE sdk FAIL the loader is SDK %u", api->version); return 1; }
    LOG("PROBE sdk ok");
    hf_on_stratagem(mod, Called, NULL, 0);
    return 0;
}
