/* Pod break probe (in-game test only, never shipped; tests/in-game/pod-break.json): a mod's support weapon dropped from a
 * broken hellpod. An EAT-17 copy (register_entity with own_all, as the Stratagem Compendium's Expendables are) sits in
 * both slots of a copy of the EAT-17's hellpod rack; a second stratagem brings the vanilla rack with two EAT-17s. On the
 * ship both go in the loadout (slots 0 and 1, as stratagem-uses-probe fills a slot); the harness calls them in. In the
 * mission each rack is broken 20 s after it spawns by the game's own rack death (game.dll 0x938650, what SetLifeState
 * runs when a rack dies: its items fall, 0x936530), and the dropped launchers' InteractableComponent instances are read:
 * the interactions they have and which are on (the mask 0x936530 sets: the pickup of the item's armory list, 0x11E7AE0).
 * The copy must fall with the EAT-17's pickup on. "PROBE <check> ok" or "PROBE <check> FAIL <why>" per check. */
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "hellforge_mod.h"
#include "hellforge_content.h"

static HfMod* g_mod;
static const HfApi* hf;
#define LOG(...) hf->log(g_mod, __VA_ARGS__)

#define EAT17_RACK 0x0DC7A18342B62BECull      /* weapon_rack_lat_oneshot: the EAT-17's two launchers */
#define SUPPORT_HELLPOD 0x73F8498BFFDCF415ull /* the hellpod support weapons come in */
#define COPY_CODE "LRLRLRLRD"
#define VANILLA_CODE "LRLRLRLRU"
#define BREAK_AFTER 20000u /* ms after the rack spawns */
#define READ_AFTER 3000u   /* ms after the break */

/* game.dll 25480438 */
#define RACK_DIES 0x938650u     /* void (racks, rack): 0x9382A0 (items flagged to go with the rack removed, the rest fall), then the drop RPC */
static const uint8_t kRackDiesPin[24] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x3B, 0x15, 0xD4, 0xB5, 0xB4, 0x02,
    0x8B, 0xDA, 0x48, 0x8B, 0xF9, 0x0F, 0x84, 0x87};
#define INTERACTABLES 0x3326D68u /* InteractableComponent's manager: map +0x14050 (capacity +0x14058, empty key +0x1405C,
                                  * multiplier +0x14060), instances +0x14070 (0x370: interactions of 0x68 from +0x44, their type at +0,
                                  * count +0x340), masks +0x14078 (0x10: the interactions on, field 0x788b9753) */
/* loadout_scrub.c (stratagem-uses-probe's) */
#define LS_HISTORY 0x347CE50u
#define LS_PROFILE 0x347CDD8u
#define LS_PEER 0x347CEF0u
#define LS_MANAGER 0x347CEF8u
#define LS_RECORDS 0x2D200u
#define LS_RECORD 0x1690u
#define LS_LOADOUT 0x38u
#define LS_SLOTS 0x188u
#define LS_SLOT 0x30u
#define LS_SLOT_COUNT 0x788u
#define LS_PAIRS 0x4Cu

typedef void (*RackDiesFn)(void* racks, uint32_t rack);

typedef struct Pod
{
    const char* label;
    uint64_t rackResource;
    uint32_t stratagem;
    uint64_t itemResource;
    uint32_t rack, items[2];
    int itemCount, broken, read, held, listed;
    ULONGLONG seen, brokenAt;
    uint64_t on; /* the interaction types on after the drop, one bit per type below 64 */
} Pod;

static HfEntityDef g_copyDef, g_rackDef;
static HfSet g_rackSet[3];
static uint64_t g_copy, g_payload[2][2];
static Pod g_pods[2] = {{.label = "copy"}, {.label = "vanilla"}}; /* the copy's rack first: loadout slot 0 */
static int g_set, g_inMission, g_verdict;

static uintptr_t Pointer(uintptr_t at) { uintptr_t v = 0; return at != 0 && hf->read(at, &v, 8) == 0 ? v : 0; }
static uint32_t U32(uintptr_t at) { uint32_t v = 0; return at != 0 && hf->read(at, &v, 4) == 0 ? v : 0xFFFFFFFFu; }

/* The value index of key in one of the game's open-addressed maps ({u32 key, u32 value}), or ~0u. */
static uint32_t MapFind(uintptr_t map, uint32_t capacity, uint32_t empty, uint32_t multiplier, uint32_t key)
{
    if (map == 0 || capacity == 0 || capacity > (1u << 20) || (capacity & (capacity - 1)) != 0) return 0xFFFFFFFFu;
    for (uint32_t i = 0; i < capacity; i++)
    {
        uint32_t at = (i + multiplier * key) & (capacity - 1), found = U32(map + (uintptr_t)at * 8);
        if (found == key) return U32(map + (uintptr_t)at * 8 + 4);
        if (found == empty) return 0xFFFFFFFFu;
    }
    return 0xFFFFFFFFu;
}

/* An item's interactions as text ("3 13 [on: 3]"), and the types on as bits; 0 when it has no InteractableComponent. */
static int Interactions(uintptr_t game, uint32_t entity, char* text, size_t size, uint64_t* on)
{
    uintptr_t m = Pointer(game + INTERACTABLES);
    uint32_t index = m ? MapFind(Pointer(m + 0x14050), U32(m + 0x14058), U32(m + 0x1405C), U32(m + 0x14060), entity) : 0xFFFFFFFFu;
    if (index == 0xFFFFFFFFu) return 0;
    uintptr_t instance = Pointer(m + 0x14070) + (uintptr_t)index * 0x370;
    uint32_t count = U32(instance + 0x340), mask = U32(Pointer(m + 0x14078) + (uintptr_t)index * 0x10);
    if (count > 32) return 0;
    size_t n = (size_t)snprintf(text, size, "types");
    *on = 0;
    for (uint32_t k = 0; k < count && n < size; k++) n += (size_t)snprintf(text + n, size - n, " %u", U32(instance + 0x44 + (uintptr_t)k * 0x68));
    if (n < size) n += (size_t)snprintf(text + n, size - n, ", on:");
    for (uint32_t k = 0; k < count && n < size; k++)
    {
        uint32_t type = U32(instance + 0x44 + (uintptr_t)k * 0x68);
        if ((mask >> k & 1) == 0) continue;
        if (type < 64) *on |= 1ull << type;
        n += (size_t)snprintf(text + n, size - n, " %u", type);
    }
    return 1;
}

/* The racks manager and the rack's record index, or 0. */
static uintptr_t RackRecord(uintptr_t game, uint32_t rack, uint32_t* index)
{
    uintptr_t cell = hf->global(g_mod, "HellpodRackManager"), racks = cell ? Pointer(cell) : 0;
    *index = racks ? MapFind(Pointer(racks + 0x28), U32(racks + 0x30), U32(racks + 0x34), U32(racks + 0x38), rack) : 0xFFFFFFFFu;
    (void)game;
    return *index != 0xFFFFFFFFu ? racks : 0;
}

/* The items on the rack's slots (network ids at +0x50 + 0x2C * index + 8, 0x7FFF none), as entities; kept when it holds
 * none now. 0 when the rack is not in the rack manager. */
static int RackItems(uintptr_t game, Pod* p)
{
    uint32_t index, items[2];
    int count = 0;
    uintptr_t racks = RackRecord(game, p->rack, &index);
    for (uint32_t k = 0; racks && k < 8 && count < 2; k++)
    {
        uint32_t network = U32(Pointer(racks + 0x50) + (uintptr_t)index * 0x2C + 8 + k * 4);
        uint32_t entity = network != 0x7FFFu && network != 0xFFFFFFFFu ? hf->entity_of_network(g_mod, network) : 0xFFFFFFFFu;
        if (entity != 0xFFFFFFFFu) items[count++] = entity;
    }
    if (count > 0) { memcpy(p->items, items, sizeof items); p->itemCount = count; }
    return racks != 0;
}

static void LogItems(uintptr_t game, const Pod* p, const char* when, int keep)
{
    for (int k = 0; k < p->itemCount; k++)
    {
        char text[256];
        uint64_t on = 0;
        if (Interactions(game, p->items[k], text, sizeof text, &on)) LOG("PROBE %s item %u %s: %s", p->label, p->items[k], when, text);
        else LOG("PROBE %s item %u %s: no interactable", p->label, p->items[k], when);
        if (keep) ((Pod*)p)->on |= on;
    }
}

/* The game's rack death on the rack, with the code checked first. */
static void BreakRack(uintptr_t game, Pod* p)
{
    uint8_t code[sizeof kRackDiesPin];
    uint32_t index;
    uintptr_t racks = RackRecord(game, p->rack, &index);
    p->broken = 1;
    p->brokenAt = GetTickCount64();
    if (hf->read(game + RACK_DIES, code, sizeof code) || memcmp(code, kRackDiesPin, sizeof code) != 0) { LOG("PROBE %s break FAIL the rack death is not the reviewed code", p->label); return; }
    if (racks == 0) { LOG("PROBE %s break FAIL rack %u is not in the rack manager", p->label, p->rack); return; }
    ((RackDiesFn)(void*)(game + RACK_DIES))((void*)racks, p->rack);
    LOG("PROBE %s break ok: rack %u (record %u) died by the game's own rack death", p->label, p->rack, index);
}

static void Verdict(void)
{
    const Pod *copy = &g_pods[0], *vanilla = &g_pods[1];
    if (g_verdict || !copy->read || !vanilla->read) return;
    g_verdict = 1;
    if (vanilla->on != 0) LOG("PROBE vanilla pickup ok: the dropped EAT-17s have a pickup on (types 0x%llx)", (unsigned long long)vanilla->on);
    else LOG("PROBE vanilla pickup FAIL the dropped EAT-17s have no pickup on");
    if (copy->on != 0 && copy->on == vanilla->on) LOG("PROBE copy pickup ok: the dropped copies have the EAT-17's pickup on (types 0x%llx)", (unsigned long long)copy->on);
    else LOG("PROBE copy pickup FAIL the dropped copies have types 0x%llx on, the EAT-17s 0x%llx", (unsigned long long)copy->on, (unsigned long long)vanilla->on);
}

static void WatchPods(uintptr_t game)
{
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < 2; i++)
    {
        Pod* p = &g_pods[i];
        if (p->seen == 0 || p->read) continue;
        if (!p->broken)
        {
            int held = RackItems(game, p);
            if (held && !p->held) p->held = 1;
            if (p->held && p->itemCount > 0 && !p->listed && now - p->seen >= 2000)
            {
                p->listed = 1;
                LOG("PROBE %s rack %u holds %d item(s)", p->label, p->rack, p->itemCount);
                LogItems(game, p, "on the rack", 0);
            }
            if (!held && (p->held || now - p->seen >= 5000))
            {
                p->broken = 1;
                p->brokenAt = now;
                LOG("PROBE %s rack broke by itself: rack %u %s the rack manager %llu ms after it spawned (%d item(s) known)", p->label,
                    p->rack, p->held ? "left" : "was never in", (unsigned long long)(now - p->seen), p->itemCount);
            }
            else if (now - p->seen >= BREAK_AFTER) BreakRack(game, p);
        }
        else if (p->broken && now - p->brokenAt >= READ_AFTER)
        {
            p->read = 1;
            LogItems(game, p, "dropped", 1);
        }
    }
    Verdict();
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

/* On the ship, once logged in: the copy's stratagem in slot 0, the vanilla rack's in slot 1, their stable ids in pairs 0 and 1. */
static void FillLoadout(uintptr_t game)
{
    uintptr_t manager = Pointer(game + LS_MANAGER);
    if (manager == 0 || U32(manager + 0xDDCF8) != 12) return;
    uintptr_t loadout = LocalLoadout(game), profile = Pointer(game + LS_PROFILE);
    uint32_t slots = U32(loadout + LS_SLOT_COUNT);
    if (loadout == 0 || profile == 0 || slots < 2 || slots > 32) return;
    uintptr_t rows[2];
    for (int k = 0; k < 2; k++)
    {
        rows[k] = hf->stratagem_row(g_pods[k].stratagem);
        if (rows[k] == 0 || U32(rows[k]) != g_pods[k].stratagem) return;
    }
    g_set = 1;
    for (int k = 0; k < 2; k++)
    {
        uintptr_t slot = loadout + LS_SLOTS + (uintptr_t)k * LS_SLOT;
        uint32_t type = g_pods[k].stratagem, stable = U32(rows[k] + 4), uses = U32(rows[k] + 0x50);
        uint8_t picked = 1;
        uint64_t ready = 0;
        if (hf->write(slot, &type, 4) || hf->write(slot + 4, &uses, 4) || hf->write(slot + 9, &picked, 1) || hf->write(slot + 0x18, &ready, 8) ||
            hf->write(profile + LS_PAIRS + (uintptr_t)k * 8, &stable, 4))
        { LOG("PROBE loadout FAIL could not write slot %d", k); return; }
    }
    LOG("PROBE loadout ok: slot 0 type %u (the copy's rack), slot 1 type %u (the EAT-17's rack)", U32(loadout + LS_SLOTS), U32(loadout + LS_SLOTS + LS_SLOT));
}

static void Tick(HfMod* mod, float dt, void* user)
{
    (void)mod; (void)dt; (void)user;
    uintptr_t game = hf->module_base("game.dll", NULL);
    uint32_t state = hf->game_state(g_mod);
    if (game == 0) return;
    if (state == HF_STATE_MISSION) { g_inMission = 1; WatchPods(game); return; }
    if (g_inMission && !g_verdict && state == HF_STATE_SHIP)
    {
        g_verdict = 1;
        LOG("PROBE pods FAIL the mission ended before both racks broke and dropped their items (copy %s, vanilla %s)",
            g_pods[0].read ? "read" : "not read", g_pods[1].read ? "read" : "not read");
    }
    if (!g_set && state == HF_STATE_SHIP) FillLoadout(game);
}

/* In the mission: each rack once, by its resource; its items too, should the rack go before they are read from it. */
static void Spawned(HfMod* mod, const HfSpawnEvent* spawn, void* user)
{
    (void)mod; (void)user;
    for (int i = 0; i < 2; i++)
    {
        Pod* p = &g_pods[i];
        if (spawn->resource == p->itemResource && !p->held && p->itemCount < 2)
        {
            p->items[p->itemCount++] = spawn->entity;
            LOG("PROBE %s item spawned: entity %u", p->label, spawn->entity);
        }
        if (spawn->resource != p->rackResource || p->seen != 0) continue;
        p->rack = spawn->entity;
        p->seen = GetTickCount64();
        LOG("PROBE %s rack spawned ok: entity %u (network %u, authority %u)", p->label, spawn->entity, spawn->network, spawn->authority);
    }
}

/* What killed a rack, when one dies. */
static void Killed(HfMod* mod, const HfKillEvent* kill, void* user)
{
    (void)mod; (void)user;
    for (int i = 0; i < 2; i++)
        if (g_pods[i].seen != 0 && kill->victim == g_pods[i].rack)
            LOG("PROBE %s rack %u killed (dealer %u)", g_pods[i].label, kill->victim, kill->dealer);
}

static uint32_t AddStratagem(HfMod* mod, const char* name, const char* title, const char* code, uint64_t* payload, uint64_t rack)
{
    payload[0] = rack;
    payload[1] = SUPPORT_HELLPOD;
    HfStratagem s = {.size = sizeof s};
    s.base = HF_STRATAGEM_EAT17_EXPENDABLE_ANTITANK;
    s.name = name;
    s.title = title;
    s.code = code;
    s.payload = payload;
    s.payload_count = 2;
    return hf->add_stratagem(mod, &s);
}

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod)
{
    hf = api;
    g_mod = mod;
    if (api->version < 16 || api->size < offsetof(HfApi, entity_of_network) + sizeof(void*)) { api->log(mod, "PROBE sdk FAIL the loader is SDK %u", api->version); return 1; }
    LOG("PROBE sdk ok (SDK %u)", api->version);
    g_copyDef = (HfEntityDef){.size = sizeof(HfEntityDef), .name = "hellforge/pod_break_probe/eat_copy", .base = HF_ENTITY_EAT17_EXPENDABLE_ANTITANK,
        .own_all = 1, .title = "Probe Launcher", .designation = "EAT-P"};
    g_copy = hf->register_entity(mod, &g_copyDef);
    g_rackSet[0] = (HfSet){HF_FIELD(HellpodRack, slot0_item), HF_U64(g_copy)};
    g_rackSet[1] = (HfSet){HF_FIELD(HellpodRack, slot1_item), HF_U64(g_copy)};
    g_rackDef = (HfEntityDef){.size = sizeof(HfEntityDef), .name = "hellforge/pod_break_probe/eat_rack", .base = EAT17_RACK, .own_all = 1, .set = g_rackSet};
    g_pods[0].rackResource = g_copy ? hf->register_entity(mod, &g_rackDef) : 0;
    g_pods[1].rackResource = EAT17_RACK;
    g_pods[0].itemResource = g_copy;
    g_pods[1].itemResource = HF_ENTITY_EAT17_EXPENDABLE_ANTITANK;
    g_pods[0].stratagem = g_pods[0].rackResource ? AddStratagem(mod, "hellforge/pod_break_probe/copy_pod", "Probe Launcher Pod", COPY_CODE, g_payload[0], g_pods[0].rackResource) : 0;
    g_pods[1].stratagem = AddStratagem(mod, "hellforge/pod_break_probe/vanilla_pod", "EAT-17 Probe Pod", VANILLA_CODE, g_payload[1], EAT17_RACK);
    LOG("PROBE added %s: copy %016llx, its rack %016llx, stratagems %u and %u", g_copy && g_pods[0].rackResource && g_pods[0].stratagem && g_pods[1].stratagem ? "ok" : "FAIL",
        (unsigned long long)g_copy, (unsigned long long)g_pods[0].rackResource, g_pods[0].stratagem, g_pods[1].stratagem);
    hf_on_spawn(mod, Spawned, NULL, 0);
    hf_on_kill(mod, Killed, NULL, 0);
    LOG(hf_on_game_thread(mod, Tick, NULL) == 0 ? "PROBE game_thread ok" : "PROBE game_thread FAIL refused");
    return 0;
}
