/* Stratagem uses probe (in-game test only, never shipped; tests/in-game/stratagem-uses.json). Its stats give the Orbital
 * Railcannon Strike (type 58) 3 uses before cooldown (native/mod-loader/stratagem_uses.c). On the ship it puts the
 * Railcannon in the player's loadout before the mission, as stratagem-cap-probe does (the local player's PlayerHistory
 * slot 0 and the profile's first pair, game build 25480438); in the mission it counts the Railcannon's beacons: the
 * harness calls it four times, 20 s apart, and only three may land before the cooldown. "PROBE <check> ok" or
 * "PROBE <check> FAIL <why>" per check. */
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "hellforge_mod.h"
#include "hellforge_content.h"

static HfMod* g_mod;
static const HfApi* hf;
#define LOG(...) hf->log(g_mod, __VA_ARGS__)

#define PROBE_TYPE HF_STRATAGEM_ORBITAL_RAILCANNON_STRIKE
#define PROBE_STABLE 0xA39552A5u /* its stable id (hellforge_content.h): stratagem_row is for added stratagems only */
static int g_set, g_beacons;

/* loadout_scrub.c */
#define LS_HISTORY 0x347CE50u
#define LS_PROFILE 0x347CDD8u
#define LS_PEER 0x347CEF0u
#define LS_MANAGER 0x347CEF8u
#define LS_RECORDS 0x2D200u
#define LS_RECORD 0x1690u
#define LS_LOADOUT 0x38u
#define LS_SLOTS 0x188u
#define LS_SLOT_COUNT 0x788u
#define LS_PAIRS 0x4Cu

static uintptr_t Pointer(uintptr_t at) { uintptr_t v = 0; return at != 0 && hf->read(at, &v, 8) == 0 ? v : 0; }
static uint32_t U32(uintptr_t at) { uint32_t v = 0; return at != 0 && hf->read(at, &v, 4) == 0 ? v : 0xFFFFFFFFu; }

/* The local player's loadout in PlayerHistory, or 0. */
static uintptr_t LocalLoadout(uintptr_t game)
{
    uintptr_t history = Pointer(game + LS_HISTORY), peerHolder = Pointer(game + LS_PEER);
    if (history == 0 || peerHolder == 0) return 0;
    uint64_t peer = 0;
    if (hf->read(peerHolder + 0xB398, &peer, 8) || peer == 0) return 0;
    uint32_t records = U32(history + LS_RECORDS);
    for (uint32_t r = 0; r < records && r < 32; r++)
    {
        uint64_t id = 0;
        if (!hf->read(history + (uintptr_t)r * LS_RECORD, &id, 8) && id == peer) return history + (uintptr_t)r * LS_RECORD + LS_LOADOUT;
    }
    return 0;
}

/* On the ship, once logged in and the stratagems are in: the Railcannon in slot 0 and its stable id in pair 0. */
static void Tick(HfMod* mod, float dt, void* user)
{
    (void)mod; (void)dt; (void)user;
    if (g_set || hf->game_state(g_mod) != HF_STATE_SHIP) return;
    uintptr_t game = hf->module_base("game.dll", NULL);
    uintptr_t manager = Pointer(game + LS_MANAGER);
    if (game == 0 || manager == 0 || U32(manager + 0xDDCF8) != 12) return;
    uintptr_t loadout = LocalLoadout(game), profile = Pointer(game + LS_PROFILE);
    if (loadout == 0 || profile == 0) return;
    uint32_t type = PROBE_TYPE, stable = PROBE_STABLE, slots = U32(loadout + LS_SLOT_COUNT);
    if (slots == 0 || slots > 32) return;
    g_set = 1;
    if (hf->write(loadout + LS_SLOTS, &type, 4) || hf->write(profile + LS_PAIRS, &stable, 4)) { LOG("PROBE loadout FAIL could not write it"); return; }
    LOG("PROBE loadout ok: slot 0 type %u, stable id %08x", U32(loadout + LS_SLOTS), stable);
}

static void Called(HfMod* mod, const HfStratagemEvent* event, void* user)
{
    (void)mod; (void)user;
    if (event->type != PROBE_TYPE) { LOG("PROBE beacon of type %u", event->type); return; }
    g_beacons++;
    LOG("PROBE railcannon beacon %d", g_beacons);
    if (g_beacons == 3) LOG("PROBE burst ok: three calls before the cooldown");
    if (g_beacons == 4) LOG("PROBE burst FAIL a fourth call got in before the cooldown");
}

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod)
{
    hf = api;
    g_mod = mod;
    if (api->version < 16) { api->log(mod, "PROBE sdk FAIL the loader is SDK %u", api->version); return 1; }
    LOG("PROBE sdk ok");
    LOG(hf_on_game_thread(mod, Tick, NULL) == 0 ? "PROBE game_thread ok" : "PROBE game_thread FAIL refused");
    hf_on_stratagem(mod, Called, NULL, 0);
    return 0;
}
