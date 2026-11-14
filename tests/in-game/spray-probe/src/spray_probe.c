/* Spray probe (in-game test only, never shipped; tests/in-game/spray20.json): SDK 20's HfWeapon.spray_damage in the real
 * game. Two copies of the FLAM-40 Flamethrower: "hot", spraying a damage of its own (the FLAM-40's at 3 times), and
 * "gas", spraying the TX-41 Sterilizer's damage (its gas status). Each gets its own SprayWeaponComponentData record
 * with the damage in it (+0xC8); the FLAM-40's own record keeps 14. A stratagem calls the hot copy in (the FLAM-40
 * stratagem with the copy as its payload); on the ship the probe puts it in loadout slot 0 as stratagem-uses-probe
 * does, so the harness can call it, and in the mission it logs the copy's spawn and every hit its damage deals.
 * "PROBE <check> ok" or "PROBE <check> FAIL <why>" per check. */
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "hellforge_mod.h"
#include "hellforge_content.h"

static HfMod* g_mod;
static const HfApi* hf;
#define LOG(...) hf->log(g_mod, __VA_ARGS__)

#define HOT_UNIT "hellforge/spray_probe/flamethrower_hot"
#define GAS_UNIT "hellforge/spray_probe/flamethrower_gas"
#define SUPPORT_HELLPOD 0x73F8498BFFDCF415ull /* the hellpod support weapons come in (docs/modding/content.md) */
#define SPRAY_DAMAGE_AT 0xC8u

static uint32_t g_hotDamage, g_hotItem, g_gasItem, g_stratagem;
static uint64_t g_hotEntity, g_gasEntity, g_payload[2];
static int g_set, g_spawned, g_hits;
static uint32_t g_hotNetwork = 0x7FFFu; /* the spawned hot copy's network id */

/* loadout_scrub.c (stratagem-uses-probe's) */
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

/* The SprayWeapon damage an entity's record holds now (get_live: shared or not), or ~0u. */
static uint32_t SprayDamage(uint64_t entity)
{
    HfField field = HF_FIELD(SprayWeapon, damage);
    HfLiveValue value;
    return hf->get_live(g_mod, entity, &field, &value) == 0 ? (uint32_t)value.now.number : 0xFFFFFFFFu;
}

/* Once the entity data is in: each copy's record is its own (component_record finds only own records) and holds its
 * damage; the FLAM-40's holds 14. */
static void EntitiesReady(HfMod* mod, HfEvent event, void* user)
{
    (void)mod; (void)event; (void)user;
    uint32_t stride = 0;
    uintptr_t hot = hf->component_record("SprayWeaponComponentData", g_hotEntity, &stride);
    uintptr_t gas = hf->component_record("SprayWeaponComponentData", g_gasEntity, &stride);
    uint32_t hotDamage = hot ? U32(hot + SPRAY_DAMAGE_AT) : 0, gasDamage = gas ? U32(gas + SPRAY_DAMAGE_AT) : 0, base = SprayDamage(HF_ENTITY_FLAM40_FLAMETHROWER);
    if (hot && stride == 224 && hotDamage == g_hotDamage) LOG("PROBE hot record ok: its own SprayWeapon record (stride %u), damage %u", stride, hotDamage);
    else LOG("PROBE hot record FAIL record %p stride %u damage %u (want %u)", (void*)hot, stride, hotDamage, g_hotDamage);
    if (gas && gasDamage == HF_DAMAGE_TX41_STERILIZER) LOG("PROBE gas record ok: its own SprayWeapon record, damage %u", gasDamage);
    else LOG("PROBE gas record FAIL record %p damage %u (want %u)", (void*)gas, gasDamage, HF_DAMAGE_TX41_STERILIZER);
    if (base == HF_DAMAGE_FLAM40_FLAMETHROWER) LOG("PROBE base unchanged ok: the FLAM-40 sprays damage %u", base);
    else LOG("PROBE base unchanged FAIL the FLAM-40 sprays damage %u", base);
}

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

/* On the ship, once logged in and the stratagems are in: the hot copy's stratagem in slot 0 and its stable id in pair 0. */
static void Tick(HfMod* mod, float dt, void* user)
{
    (void)mod; (void)dt; (void)user;
    if (g_set || g_stratagem == 0 || hf->game_state(g_mod) != HF_STATE_SHIP) return;
    uintptr_t game = hf->module_base("game.dll", NULL);
    uintptr_t manager = Pointer(game + LS_MANAGER);
    if (game == 0 || manager == 0 || U32(manager + 0xDDCF8) != 12) return;
    uintptr_t loadout = LocalLoadout(game), profile = Pointer(game + LS_PROFILE), row = hf->stratagem_row(g_stratagem);
    if (loadout == 0 || profile == 0 || row == 0) return;
    uint32_t type = g_stratagem, stable = U32(row + 4), slots = U32(loadout + LS_SLOT_COUNT), uses = U32(row + 0x50);
    if (slots == 0 || slots > 32 || U32(row) != g_stratagem) return;
    g_set = 1;
    uint8_t picked = 1;
    uint64_t ready = 0;
    if (hf->write(loadout + LS_SLOTS, &type, 4) || hf->write(loadout + LS_SLOTS + 4, &uses, 4) || hf->write(loadout + LS_SLOTS + 9, &picked, 1) ||
        hf->write(loadout + LS_SLOTS + 0x18, &ready, 8) || hf->write(profile + LS_PAIRS, &stable, 4))
    { LOG("PROBE loadout FAIL could not write it"); return; }
    LOG("PROBE loadout ok: slot 0 type %u (the hot flamethrower's stratagem), stable id %08x", U32(loadout + LS_SLOTS), stable);
}

/* In the mission: the hot copy spawned from its hellpod. */
static void Spawned(HfMod* mod, const HfSpawnEvent* spawn, void* user)
{
    (void)mod; (void)user;
    if (spawn->resource != g_hotEntity || g_spawned++) return;
    g_hotNetwork = spawn->network;
    LOG("PROBE hot spawned ok: entity %u (network %u) from its hellpod", spawn->entity, spawn->network);
}

/* Hits the hot copy deals (dealer or owner its network id), when someone sprays enemies with it: the first five logged. */
static void Damaged(HfMod* mod, const HfDamageEvent* damage, void* user)
{
    (void)mod; (void)user;
    if (g_hotNetwork == 0x7FFFu || (damage->dealer != g_hotNetwork && damage->owner != g_hotNetwork)) return;
    if (g_hits++ < 5) LOG("PROBE hot hit ok: %d damage to %u (kind %u, element %u)", damage->amount, damage->target, damage->kind, damage->element);
}

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod)
{
    hf = api;
    g_mod = mod;
    if (api->version < 20 || api->size < offsetof(HfApi, ui_close) + sizeof(void*)) { api->log(mod, "PROBE sdk FAIL the loader is SDK %u", api->version); return 1; }
    LOG("PROBE sdk ok (SDK %u)", api->version);
    HfDamage hot = {.size = sizeof hot};
    hot.base = HF_DAMAGE_FLAM40_FLAMETHROWER;
    hot.name = "hot_flame";
    hot.scale = 3.0f;
    g_hotDamage = hf->add_damage(mod, &hot);
    HfWeapon flamer = {.size = sizeof flamer};
    flamer.base = HF_ITEM_FLAM40_FLAMETHROWER;
    flamer.unit = HOT_UNIT;
    flamer.designation = "FLAM-40H";
    flamer.name = "Hot Flamethrower";
    flamer.spray_damage = g_hotDamage;
    g_hotItem = hf->add_weapon(mod, &flamer);
    flamer.unit = GAS_UNIT;
    flamer.designation = "FLAM-40G";
    flamer.name = "Gas Flamethrower";
    flamer.spray_damage = HF_DAMAGE_TX41_STERILIZER;
    g_gasItem = hf->add_weapon(mod, &flamer);
    g_hotEntity = hf->resource_hash(HOT_UNIT);
    g_gasEntity = hf->resource_hash(GAS_UNIT);
    LOG("PROBE added %s: damage %u, items %08x and %08x", g_hotDamage && g_hotItem && g_gasItem ? "ok" : "FAIL", g_hotDamage, g_hotItem, g_gasItem);
    g_payload[0] = g_hotEntity;
    g_payload[1] = SUPPORT_HELLPOD;
    HfStratagem call = {.size = sizeof call};
    call.base = HF_STRATAGEM_FLAM40_FLAMETHROWER;
    call.name = "hellforge/spray_probe/stratagem_hot";
    call.title = "Hot Flamethrower";
    call.code = "LRLRLRLRD";
    call.payload = g_payload;
    call.payload_count = 2;
    call.loads_item = g_hotItem;
    g_stratagem = hf->add_stratagem(mod, &call);
    LOG("PROBE stratagem %s: type %u", g_stratagem ? "ok" : "FAIL", g_stratagem);
    hf->on(mod, HF_EVENT_ENTITIES_READY, EntitiesReady, NULL);
    hf_on_spawn(mod, Spawned, NULL, 0);
    hf_on_damage(mod, Damaged, NULL, 0);
    LOG(hf_on_game_thread(mod, Tick, NULL) == 0 ? "PROBE game_thread ok" : "PROBE game_thread FAIL refused");
    return 0;
}
