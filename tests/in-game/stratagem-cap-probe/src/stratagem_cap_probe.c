/* Stratagem cap probe (in-game test only, never shipped; tests/in-game/stratagem-cap.json): adds 300 stratagems, types
 * 150 to 449, so the co-op stratagem field must travel in 16 bits. On the ship it puts the last one (type 449, past 255)
 * in the player's loadout before the mission, where the loadout screen puts a pick: the local player's PlayerHistory
 * record's first stratagem slot and the profile's first saved pair (native/mod-loader/loadout_scrub.c has the layout,
 * game build 25480438). The mission then loads it as it loads any pick. Its input is unique (UUUUDDDDL), so the harness
 * can call it in; the beacon's type is logged. "PROBE <check> ok" or "PROBE <check> FAIL <why>" per check. */
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "hellforge_mod.h"
#include "hellforge_content.h"

static HfMod* g_mod;
static const HfApi* hf;
#define LOG(...) hf->log(g_mod, __VA_ARGS__)

#define PROBE_COUNT 300u
#define PROBE_CODE "UUUUDDDDL"
static uint32_t g_types[PROBE_COUNT];
static uint32_t g_last;
static int g_set, g_missionSeen, g_called;

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

/* On the ship, once logged in and the stratagems are in: the last type in slot 0 and its stable id in pair 0. */
static void Tick(HfMod* mod, float dt, void* user)
{
    if (g_set || g_last == 0 || hf->game_state(g_mod) != HF_STATE_SHIP) return;
    uintptr_t row = hf->stratagem_row(g_last);
    uintptr_t game = hf->module_base("game.dll", NULL);
    uintptr_t manager = Pointer(game + LS_MANAGER);
    if (row == 0 || game == 0 || manager == 0 || U32(manager + 0xDDCF8) != 12) return;
    uintptr_t loadout = LocalLoadout(game), profile = Pointer(game + LS_PROFILE);
    if (loadout == 0 || profile == 0) return;
    uint32_t stable = U32(row + 4), slots = U32(loadout + LS_SLOT_COUNT), before = U32(loadout + LS_SLOTS);
    if (slots == 0 || slots > 32) return;
    if (hf->write(loadout + LS_SLOTS, &g_last, 4) || hf->write(profile + LS_PAIRS, &stable, 4))
    {
        LOG("PROBE loadout FAIL could not write it");
        g_set = 1;
        return;
    }
    g_set = 1;
    LOG("PROBE loadout ok: slot 0 type %u (was %u; %u slots), stable id %08x", U32(loadout + LS_SLOTS), before, slots, stable);
}

static void MissionStarted(HfMod* mod, const HfStateEvent* state, void* user)
{
    g_missionSeen = 1;
    LOG("PROBE mission started%s", g_set ? "" : " FAIL without the loadout set");
    uintptr_t game = hf->module_base("game.dll", NULL), loadout = game ? LocalLoadout(game) : 0;
    uint32_t slots = loadout ? U32(loadout + LS_SLOT_COUNT) : 0;
    char line[160] = "";
    for (uint32_t k = 0; k < slots && k < 8; k++)
        snprintf(line + strlen(line), sizeof line - strlen(line), " %u", U32(loadout + LS_SLOTS + (uintptr_t)k * 0x30));
    LOG("PROBE mission loadout:%s (%u slots)", line, slots);
}

static void Called(HfMod* mod, const HfStratagemEvent* event, void* user)
{
    if (event->type == g_last && !g_called) { g_called = 1; LOG("PROBE called ok: beacon of type %u", event->type); }
    else LOG("PROBE beacon of type %u", event->type);
}

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod)
{
    hf = api;
    g_mod = mod;
    if (api->version < 16) { api->log(mod, "PROBE sdk FAIL the loader is SDK %u", api->version); return 1; }
    char name[32], title[32];
    uint32_t added = 0;
    for (uint32_t i = 0; i < PROBE_COUNT; i++)
    {
        snprintf(name, sizeof name, "cap-%u", i);
        snprintf(title, sizeof title, "Cap Test %u", i);
        HfStratagem s = {sizeof s};
        s.base = HF_STRATAGEM_ORBITAL_PRECISION_STRIKE;
        s.name = name;
        s.title = title;
        s.description = "Stratagem cap probe";
        s.code = i == PROBE_COUNT - 1 ? PROBE_CODE : NULL;
        g_types[i] = api->add_stratagem(mod, &s);
        added += g_types[i] != 0;
    }
    g_last = g_types[PROBE_COUNT - 1];
    LOG(added == PROBE_COUNT ? "PROBE added ok: %u stratagems, types %u to %u" : "PROBE added FAIL: %u stratagems, types %u to %u",
        added, g_types[0], g_last);
    LOG(hf_on_game_thread(mod, Tick, NULL) == 0 ? "PROBE game_thread ok" : "PROBE game_thread FAIL refused");
    hf_on_mission_start(mod, MissionStarted, NULL, 0);
    hf_on_stratagem(mod, Called, NULL, 0);
    return 0;
}
