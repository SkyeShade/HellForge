/* Stratagem codes probe (in-game test only, never shipped; tests/in-game/stratagem-codes.json). Its stats give Resupply
 * (type 33) the input code Up Up Left Right Down (packed 2231: native/mod-loader/stratagem_codes.c). In the mission it
 * logs the code the Resupply row points at, as the input matcher and the HUD read it, and every beacon's type, so the
 * harness's call of the new code can be checked. "PROBE <check> ok" or "PROBE <check> FAIL <why>" per check. */
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "hellforge_mod.h"
#include "hellforge_content.h"

static HfMod* g_mod;
static const HfApi* hf;
#define LOG(...) hf->log(g_mod, __VA_ARGS__)

static const uint32_t kWanted[5] = {1, 1, 4, 2, 3}; /* up up left right down */

static void MissionStarted(HfMod* mod, const HfStateEvent* state, void* user)
{
    (void)mod; (void)state; (void)user;
    uintptr_t row = hf->stratagem_row(HF_STRATAGEM_RESUPPLY), code = 0;
    uint32_t length = 0, directions[16] = {0};
    if (row == 0 || hf->read(row + 0x40, &code, 8) || hf->read(row + 0x48, &length, 4) || length > 16 || hf->read(code, directions, length * 4))
    { LOG("PROBE resupply code FAIL its row does not read"); return; }
    char line[64] = "";
    for (uint32_t i = 0; i < length; i++) snprintf(line + strlen(line), sizeof line - strlen(line), " %u", directions[i]);
    int same = length == 5 && !memcmp(directions, kWanted, sizeof kWanted);
    LOG(same ? "PROBE resupply code ok:%s" : "PROBE resupply code FAIL:%s (wanted 1 1 4 2 3)", line);
}

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
    hf_on_mission_start(mod, MissionStarted, NULL, 0);
    hf_on_stratagem(mod, Called, NULL, 0);
    return 0;
}
